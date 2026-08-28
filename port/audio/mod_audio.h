#ifndef PORT_MOD_AUDIO_H
#define PORT_MOD_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <battleship/vita_mod_audio.h>

#ifdef __cplusplus
extern "C" {
#endif

int portModAudioRegisterPCM(uint32_t owner,
                            const BattleShipVitaModAudioResource *resource,
                            const void *pcm_bytes, size_t pcm_size);
int portModAudioUnregisterFGM(uint32_t owner, uint16_t fgm_id);
void portModAudioUnregisterOwner(uint32_t owner);

/* Returns non-zero when a registered external FGM consumed this id. */
int portModAudioPlayFGM(uint16_t fgm_id);

/* Mix active external voices into an interleaved stereo s16 32 kHz frame. */
void portModAudioMixFrame(int16_t *stereo_pcm, int sample_count);

#ifdef __cplusplus
}
#endif

#endif /* PORT_MOD_AUDIO_H */
