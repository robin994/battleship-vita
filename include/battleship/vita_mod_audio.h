#ifndef BATTLESHIP_VITA_MOD_AUDIO_H
#define BATTLESHIP_VITA_MOD_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BATTLESHIP_VITA_MOD_AUDIO_LOOP = 1u << 0,
    BATTLESHIP_VITA_MOD_AUDIO_EXCLUSIVE = 1u << 1,
};

/* External native mods ship little-endian signed 16-bit PCM in their O2R.
 * The host mixer currently accepts the game's native 32 kHz output rate and
 * mono/stereo clips. gain_q15 uses 32768 == 1.0. */
typedef struct BattleShipVitaModAudioResource {
    uint16_t fgm_id;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t flags;
    int32_t gain_q15;
    const char *resource_path;
} BattleShipVitaModAudioResource;

#ifdef __cplusplus
}
#endif

#endif /* BATTLESHIP_VITA_MOD_AUDIO_H */
