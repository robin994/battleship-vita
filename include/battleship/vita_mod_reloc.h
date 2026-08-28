#ifndef BATTLESHIP_VITA_MOD_RELOC_H
#define BATTLESHIP_VITA_MOD_RELOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Apply the fighter figatree half-word fixup after normal relocation. */
    BATTLESHIP_VITA_MOD_RELOC_FIGHTER_FIGATREE = 1u << 0,
};

typedef struct BattleShipVitaModRelocResource {
    uint32_t file_id;
    const char *resource_path;
    uint32_t flags;
} BattleShipVitaModRelocResource;

#ifdef __cplusplus
}
#endif

#endif /* BATTLESHIP_VITA_MOD_RELOC_H */
