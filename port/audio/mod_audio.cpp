#include "mod_audio.h"

#include "../port_log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct Clip {
    uint32_t owner = 0;
    uint16_t fgmId = 0;
    uint16_t channels = 1;
    uint32_t flags = 0;
    int32_t gainQ15 = 32768;
    std::shared_ptr<std::vector<int16_t>> pcm;
};

struct Voice {
    std::shared_ptr<Clip> clip;
    size_t frame = 0;
    uint64_t serial = 0;
};

std::mutex sMutex;
std::unordered_map<uint16_t, std::vector<std::shared_ptr<Clip>>> sClips;
std::array<Voice, 16> sVoices{};
uint64_t sSerial = 1;

static int16_t clampS16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return static_cast<int16_t>(value);
}

static void stopExclusiveVoicesLocked() {
    for (auto &voice : sVoices) {
        if (voice.clip && (voice.clip->flags & BATTLESHIP_VITA_MOD_AUDIO_EXCLUSIVE)) {
            voice = Voice{};
        }
    }
}

} // namespace

extern "C" int portModAudioRegisterPCM(uint32_t owner,
                                        const BattleShipVitaModAudioResource *resource,
                                        const void *pcmBytes, size_t pcmSize) {
    if (owner == 0 || resource == nullptr || pcmBytes == nullptr || pcmSize < sizeof(int16_t) ||
        (pcmSize & 1u) != 0 || resource->resource_path == nullptr ||
        resource->sample_rate != 32000u ||
        (resource->channels != 1u && resource->channels != 2u)) {
        return -1;
    }

    const size_t samples = pcmSize / sizeof(int16_t);
    if ((samples % resource->channels) != 0) return -2;

    auto pcm = std::make_shared<std::vector<int16_t>>(samples);
    const uint8_t *src = static_cast<const uint8_t *>(pcmBytes);
    for (size_t i = 0; i < samples; ++i) {
        /* O2R audio payload is explicitly little-endian signed PCM. */
        (*pcm)[i] = static_cast<int16_t>(static_cast<uint16_t>(src[i * 2]) |
                                         (static_cast<uint16_t>(src[i * 2 + 1]) << 8));
    }

    auto clip = std::make_shared<Clip>();
    clip->owner = owner;
    clip->fgmId = resource->fgm_id;
    clip->channels = resource->channels;
    clip->flags = resource->flags;
    clip->gainQ15 = (resource->gain_q15 > 0) ? resource->gain_q15 : 32768;
    clip->pcm = std::move(pcm);

    std::lock_guard<std::mutex> lock(sMutex);
    auto &stack = sClips[resource->fgm_id];
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [owner](const auto &entry) { return entry->owner == owner; }),
                stack.end());
    stack.push_back(std::move(clip));
    return 0;
}

extern "C" int portModAudioUnregisterFGM(uint32_t owner, uint16_t fgmId) {
    std::lock_guard<std::mutex> lock(sMutex);
    const auto found = sClips.find(fgmId);
    if (found == sClips.end()) return -1;
    auto &stack = found->second;
    const size_t oldSize = stack.size();
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [owner](const auto &entry) { return entry->owner == owner; }),
                stack.end());
    const bool changed = stack.size() != oldSize;
    if (stack.empty()) sClips.erase(found);
    return changed ? 0 : -2;
}

extern "C" void portModAudioUnregisterOwner(uint32_t owner) {
    std::lock_guard<std::mutex> lock(sMutex);
    for (auto it = sClips.begin(); it != sClips.end();) {
        auto &stack = it->second;
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [owner](const auto &entry) { return entry->owner == owner; }),
                    stack.end());
        if (stack.empty()) it = sClips.erase(it);
        else ++it;
    }
    /* Existing voices retain shared ownership only while the module remains
     * loaded. Stop this owner's voices before SUPRX/O2R teardown. */
    for (auto &voice : sVoices) {
        if (voice.clip && voice.clip->owner == owner) voice = Voice{};
    }
}

extern "C" int portModAudioPlayFGM(uint16_t fgmId) {
    std::lock_guard<std::mutex> lock(sMutex);
    const auto found = sClips.find(fgmId);
    if (found == sClips.end() || found->second.empty()) return 0;

    const auto &clip = found->second.back();
    if (clip->flags & BATTLESHIP_VITA_MOD_AUDIO_EXCLUSIVE) stopExclusiveVoicesLocked();

    Voice *target = nullptr;
    for (auto &voice : sVoices) {
        if (!voice.clip) {
            target = &voice;
            break;
        }
    }
    if (target == nullptr) {
        target = &*std::min_element(sVoices.begin(), sVoices.end(),
                                    [](const Voice &a, const Voice &b) { return a.serial < b.serial; });
    }
    target->clip = clip;
    target->frame = 0;
    target->serial = sSerial++;
    port_log("SSB64: MOD_AUDIO_PLAY fgm=%u owner=%u frames=%u\n",
             (unsigned)fgmId, (unsigned)clip->owner,
             (unsigned)(clip->pcm->size() / clip->channels));
    return 1;
}

extern "C" void portModAudioMixFrame(int16_t *stereoPcm, int sampleCount) {
    if (stereoPcm == nullptr || sampleCount <= 0) return;
    std::lock_guard<std::mutex> lock(sMutex);

    for (auto &voice : sVoices) {
        if (!voice.clip || !voice.clip->pcm) continue;
        const Clip &clip = *voice.clip;
        const size_t totalFrames = clip.pcm->size() / clip.channels;

        for (int i = 0; i < sampleCount; ++i) {
            if (voice.frame >= totalFrames) {
                if (clip.flags & BATTLESHIP_VITA_MOD_AUDIO_LOOP) voice.frame = 0;
                else {
                    voice = Voice{};
                    break;
                }
            }
            if (!voice.clip) break;

            int32_t left;
            int32_t right;
            if (clip.channels == 1) {
                left = right = (*clip.pcm)[voice.frame];
            } else {
                left = (*clip.pcm)[voice.frame * 2];
                right = (*clip.pcm)[voice.frame * 2 + 1];
            }
            left = (left * clip.gainQ15) >> 15;
            right = (right * clip.gainQ15) >> 15;
            stereoPcm[i * 2] = clampS16(static_cast<int32_t>(stereoPcm[i * 2]) + left);
            stereoPcm[i * 2 + 1] = clampS16(static_cast<int32_t>(stereoPcm[i * 2 + 1]) + right);
            ++voice.frame;
        }
    }
}
