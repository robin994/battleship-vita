#include "port_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

/* Real-hardware testing repeatedly hit an auto-triggered psp2dmp with the
 * main thread stuck inside this log file's fflush -> write() syscall chain,
 * always at the exact same buffer-fill boundary (confirmed by dumping the
 * coredump's own captured stdio buffer contents - byte-identical across
 * independent runs, including after a mutex fix and a "force the first
 * flush early" fix, neither of which changed anything). The stop reason
 * (0x10006, never a real CPU exception in any crash this port has hit) is
 * consistent with an external watchdog snapshotting/killing the process
 * because the *main* thread hasn't made scheduler progress - and a
 * synchronous fprintf/fflush on real hardware's memory card is apparently
 * sometimes slow enough to trip whatever threshold that watchdog uses.
 *
 * Fix: take the game/audio/scheduler threads out of the disk-I/O path
 * entirely. port_log() only formats the line and pushes it onto a small
 * ring buffer (fast, in-memory, no I/O); a single dedicated writer thread
 * drains the queue and does the actual fputs to disk. Whatever the SD
 * card's real write latency is, it can no longer make the *watched* thread
 * (or any other caller) block on it. */

#define LOG_QUEUE_SLOTS 128
#define LOG_LINE_MAX 512

static FILE *sLogFile = NULL;

static char sLogQueue[LOG_QUEUE_SLOTS][LOG_LINE_MAX];
static int sQueueHead = 0; /* next slot to fill */
static int sQueueTail = 0; /* next slot to drain */
static int sQueueCount = 0;
static int sShutdown = 0;

static pthread_mutex_t sQueueMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sQueueCond = PTHREAD_COND_INITIALIZER;
static pthread_t sWriterThread;
static int sWriterThreadStarted = 0;

static void *LogWriterMain(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&sQueueMutex);
		while (sQueueCount == 0 && !sShutdown) {
			pthread_cond_wait(&sQueueCond, &sQueueMutex);
		}
		if (sQueueCount == 0 && sShutdown) {
			pthread_mutex_unlock(&sQueueMutex);
			break;
		}
		char line[LOG_LINE_MAX];
		memcpy(line, sLogQueue[sQueueTail], LOG_LINE_MAX);
		sQueueTail = (sQueueTail + 1) % LOG_QUEUE_SLOTS;
		sQueueCount--;
		pthread_mutex_unlock(&sQueueMutex);

		if (sLogFile != NULL) {
			fputs(line, sLogFile);
		}
	}
	if (sLogFile != NULL) {
		fflush(sLogFile);
	}
	return NULL;
}

void port_log_init(const char *path)
{
	if (sLogFile != NULL) return;
	sLogFile = fopen(path, "w");
	if (sLogFile == NULL) return;

	if (pthread_create(&sWriterThread, NULL, LogWriterMain, NULL) == 0) {
		sWriterThreadStarted = 1;
	}
}

void port_log_close(void)
{
	if (sWriterThreadStarted) {
		pthread_mutex_lock(&sQueueMutex);
		sShutdown = 1;
		pthread_cond_signal(&sQueueCond);
		pthread_mutex_unlock(&sQueueMutex);
		pthread_join(sWriterThread, NULL);
		sWriterThreadStarted = 0;
	}
	if (sLogFile != NULL) {
		fclose(sLogFile);
		sLogFile = NULL;
	}
}

int port_log_get_fd(void)
{
	if (sLogFile == NULL) return -1;
	return fileno(sLogFile);
}

void port_log(const char *fmt, ...)
{
	if (sLogFile == NULL) return;

	char formatted[LOG_LINE_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(formatted, sizeof(formatted), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&sQueueMutex);
	if (sQueueCount >= LOG_QUEUE_SLOTS) {
		/* Queue full - drop the line rather than block the caller. The
		 * whole point of this queue is to keep slow disk I/O off whatever
		 * thread is calling port_log(); blocking here to wait for room
		 * would defeat that. */
		pthread_mutex_unlock(&sQueueMutex);
		return;
	}
	memcpy(sLogQueue[sQueueHead], formatted, LOG_LINE_MAX);
	sQueueHead = (sQueueHead + 1) % LOG_QUEUE_SLOTS;
	sQueueCount++;
	pthread_cond_signal(&sQueueCond);
	pthread_mutex_unlock(&sQueueMutex);
}
