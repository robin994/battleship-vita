#include "port_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

#ifdef __vita__
#include <psp2/kernel/clib.h>
#endif

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Real-hardware testing repeatedly hit an auto-triggered psp2dmp with the
 * main thread stuck inside this log file's fflush -> write() syscall chain,
 * always at the exact same buffer-fill boundary. Moving the file write onto
 * a dedicated writer thread (so no caller ever blocks on disk I/O) fixed
 * *that* symptom, but real-hardware testing then showed a *different* one:
 * with sceClibPrintf funneled through the same single writer thread as the
 * file write (to avoid a suspected multi-thread contention issue on the
 * shared debug-output channel), the live network-captured log appeared to
 * freeze - except a one-off direct (queue-bypassing) sceClibPrintf call
 * placed further down in the boot sequence printed *before* the queued
 * lines that logically preceded it. That's only possible if the game
 * thread had already run well past the "frozen" point and the *writer
 * thread* was simply the one stuck.
 *
 * Two independent queues, two independent writer threads, fixed the case
 * where a slow file write could delay sceClibPrintf. It did NOT fix the
 * file write itself: every rebuild (single queue, dual queue, forcing the
 * very first flush to happen synchronously before the writer thread even
 * existed) reproduced the *identical* stall, always the first time enough
 * bytes had accumulated to fill libc's internal stdio buffer (1024 bytes
 * on this newlib build) - i.e. the first implicit fflush() inside fputs().
 * That points at stdio's own buffering/locking on this VitaSDK newlib
 * build, not at raw microSD latency (a multi-minute wait never completed
 * either, which is deadlock-shaped, not slow-disk-shaped).
 *
 * Fix: bypass stdio for the file writer entirely. Use a raw fd and
 * write(2) - the same async-signal-safe primitive port_watchdog.cpp
 * already relies on for its own crash-time log writes - so there is no
 * userspace buffer to fill and no stdio lock to contend on. */

#define LOG_QUEUE_SLOTS 256
#define LOG_QUEUE_PRIORITY_RESERVE 32
#define LOG_LINE_MAX 512

typedef struct {
	char lines[LOG_QUEUE_SLOTS][LOG_LINE_MAX];
	int head; /* next slot to fill */
	int tail; /* next slot to drain */
	int count;
	int shutdown;
	unsigned int dropped;
	unsigned int high_water;
	unsigned int pushed;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t thread;
	int started;
} LogQueue;

static int sLogFd = -1;
#if defined(__vita__) && defined(PORT_LOG_STDOUT)
static LogQueue sPrintQueue = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
#endif
static LogQueue sFileQueue = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static int LogLineIsPriority(const char *line)
{
	static const char *const markers[] = {
		"_REJECT", "_MISMATCH", "_STALE_DROP", " OVERFLOW",
		"_OOB", "UNDERFLOW", "QUARANTINE", "ABORT", "ANOMALY",
		"WARNING", "CAUGHT", " FAIL", "ERROR", "TASK_SEAL",
		"FAST3D_SCENE", "VITA_SCENE_RENDER"
	};
	size_t i;

	for (i = 0; i < (sizeof(markers) / sizeof(markers[0])); i++) {
		if (strstr(line, markers[i]) != NULL) {
			return 1;
		}
	}
	return 0;
}

static void QueuePush(LogQueue *q, const char *line, int priority)
{
	pthread_mutex_lock(&q->mutex);
	if (q->count >= (priority ? LOG_QUEUE_SLOTS :
	                (LOG_QUEUE_SLOTS - LOG_QUEUE_PRIORITY_RESERVE))) {
		/* Never block the caller, but make loss observable.  The old logger
		 * silently dropped the exact high-rate fighter-audit bursts needed to
		 * diagnose partial models.  Normal telemetry now leaves a small reserve
		 * for reject/mismatch/overflow markers, so saturation cannot erase the
		 * evidence that distinguishes a bad resource from a renderer failure. */
		q->dropped++;
		pthread_mutex_unlock(&q->mutex);
		return;
	}
	/* Only the empty -> non-empty transition can make the consumer runnable.
	 * Avoid redundant pthread condition-variable (and underlying kernel
	 * semaphore) bookkeeping on every line in a high-rate logging burst. */
	const int was_empty = (q->count == 0);
	size_t len = 0;
	while ((len < (LOG_LINE_MAX - 1)) && (line[len] != '\0')) len++;
	memcpy(q->lines[q->head], line, len);
	q->lines[q->head][len] = '\0';
	q->head = (q->head + 1) % LOG_QUEUE_SLOTS;
	q->count++;
	q->pushed++;
	if ((unsigned int)q->count > q->high_water) {
		q->high_water = (unsigned int)q->count;
	}
	if (was_empty) {
		pthread_cond_signal(&q->cond);
	}
	pthread_mutex_unlock(&q->mutex);
}

/* Returns 0 and fills `out` with the next line, or returns nonzero once
 * shutdown is requested and the queue has been fully drained. */
static int QueuePop(LogQueue *q, char *out)
{
	pthread_mutex_lock(&q->mutex);
	while (q->count == 0 && !q->shutdown) {
		pthread_cond_wait(&q->cond, &q->mutex);
	}
	if (q->count == 0 && q->shutdown) {
		pthread_mutex_unlock(&q->mutex);
		return 1;
	}
	memcpy(out, q->lines[q->tail], LOG_LINE_MAX);
	q->tail = (q->tail + 1) % LOG_QUEUE_SLOTS;
	q->count--;
	pthread_mutex_unlock(&q->mutex);
	return 0;
}

static void QueueShutdown(LogQueue *q)
{
	pthread_mutex_lock(&q->mutex);
	q->shutdown = 1;
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
}

#if defined(__vita__) && defined(PORT_LOG_STDOUT)
static void *PrintWriterMain(void *arg)
{
	(void)arg;
	char line[LOG_LINE_MAX];
	while (QueuePop(&sPrintQueue, line) == 0) {
		sceClibPrintf("%s", line);
	}
	return NULL;
}
#endif

/* write(2) may write fewer bytes than requested even for a regular file;
 * loop until the whole line is out (or a real error, which we can't do
 * anything about here - there's no lower-level channel to report it on). */
static void WriteAll(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
#if defined(_WIN32)
		int n = _write(fd, buf + off, (unsigned int)(len - off));
#else
		ssize_t n = write(fd, buf + off, len - off);
#endif
		if (n <= 0) return;
		off += (size_t)n;
	}
}

static void *FileWriterMain(void *arg)
{
	(void)arg;
	char line[LOG_LINE_MAX];
	while (QueuePop(&sFileQueue, line) == 0) {
		if (sLogFd >= 0) {
			WriteAll(sLogFd, line, strlen(line));
		}
	}
	return NULL;
}

void port_log_init(const char *path)
{
	if (sLogFd >= 0) return;
#if defined(_WIN32)
	_sopen_s(&sLogFd, path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
	         _SH_DENYNO, _S_IREAD | _S_IWRITE);
#else
	sLogFd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
#endif
	if (sLogFd < 0) return;

	/* Default pthread stack size on VitaSDK is tiny - real-hardware testing
	 * this session found a crashing thread's entire stack mapping was only
	 * 16KB. These threads' own call chains are shallow (format a string,
	 * call write()), but WriteAll()'s underlying newlib/syscall path is the
	 * same one that has already been seen to run deep on this platform, so
	 * give them real headroom rather than relying on the tiny default. */
	pthread_attr_t stack_attr;
	pthread_attr_init(&stack_attr);
	pthread_attr_setstacksize(&stack_attr, 64 * 1024);

	if (pthread_create(&sFileQueue.thread, &stack_attr, FileWriterMain, NULL) == 0) {
		sFileQueue.started = 1;
	}
	#if defined(__vita__) && defined(PORT_LOG_STDOUT)
	if (pthread_create(&sPrintQueue.thread, &stack_attr, PrintWriterMain, NULL) == 0) {
		sPrintQueue.started = 1;
	}
#endif
	pthread_attr_destroy(&stack_attr);
}

void port_log_close(void)
{
	if (sFileQueue.started) {
		QueueShutdown(&sFileQueue);
		pthread_join(sFileQueue.thread, NULL);
		sFileQueue.started = 0;
	}
	#if defined(__vita__) && defined(PORT_LOG_STDOUT)
	if (sPrintQueue.started) {
		QueueShutdown(&sPrintQueue);
		pthread_join(sPrintQueue.thread, NULL);
		sPrintQueue.started = 0;
	}
	#endif
	if (sLogFd >= 0) {
#if defined(_WIN32)
		_close(sLogFd);
#else
		close(sLogFd);
#endif
		sLogFd = -1;
	}
}

int port_log_get_fd(void)
{
	return sLogFd;
}

unsigned int port_log_get_dropped_lines(void)
{
	unsigned int dropped;
	pthread_mutex_lock(&sFileQueue.mutex);
	dropped = sFileQueue.dropped;
	pthread_mutex_unlock(&sFileQueue.mutex);
	return dropped;
}

unsigned int port_log_get_queue_high_water(void)
{
	unsigned int high_water;
	pthread_mutex_lock(&sFileQueue.mutex);
	high_water = sFileQueue.high_water;
	pthread_mutex_unlock(&sFileQueue.mutex);
	return high_water;
}

unsigned int port_log_get_queued_lines(void)
{
	unsigned int queued;
	pthread_mutex_lock(&sFileQueue.mutex);
	queued = (unsigned int)sFileQueue.count;
	pthread_mutex_unlock(&sFileQueue.mutex);
	return queued;
}

void port_log(const char *fmt, ...)
{
	char formatted[LOG_LINE_MAX];
	int priority;
	va_list ap;
	va_start(ap, fmt);
	/* sceClibVsnprintf, not newlib's vsnprintf: a real-hardware crash landed
	 * inside newlib's _svfprintf_r when a different call site (O2rArchive's
	 * diagnostic logging) used vsnprintf from inside the game coroutine -
	 * matching this project's established pattern of newlib internals
	 * carrying their own lazily-created, coroutine-unsafe kernel locks (see
	 * the wiki's "manually-swapped stacks" finding). port_log() is called
	 * from many places throughout boot, including from inside the
	 * coroutine, so it's exposed to the same risk even though it hasn't
	 * been directly observed crashing here - Sony's sceClibVsnprintf
	 * bypasses newlib's stdio layer entirely, consistent with this file's
	 * own established "avoid newlib stdio" approach for the actual
	 * file/console writes below. */
#ifdef __vita__
	sceClibVsnprintf(formatted, sizeof(formatted), fmt, ap);
#else
	vsnprintf(formatted, sizeof(formatted), fmt, ap);
#endif
	va_end(ap);

	/* Some snprintf implementations are allowed to leave the destination
	 * unterminated on truncation.  Every downstream consumer uses C-string
	 * semantics, so make this invariant explicit instead of letting strlen()
	 * walk into an adjacent queue slot. */
	formatted[LOG_LINE_MAX - 1] = '\0';
	priority = LogLineIsPriority(formatted);

#if defined(__vita__) && defined(PORT_LOG_STDOUT)
	if (sPrintQueue.started) {
		QueuePush(&sPrintQueue, formatted, priority);
	}
#endif
	if (sFileQueue.started) {
		QueuePush(&sFileQueue, formatted, priority);
	}
}
