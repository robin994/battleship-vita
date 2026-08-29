# PS Vita → N64 controller bindings

How a physical PS Vita button press becomes an N64 controller bit inside SSB64,
and exactly which files to edit to change a binding.

Read this before touching anything under `libultraship/src/.../controller/` or
`decomp/src/sys/controller.c` for the Vita port.

---

## The signal path — four layers

```
physical Vita control
   │   (SDL VitaSDK joystick driver — FIXED, not ours)
   ▼
SDL joystick button index / axis index      b0..b15 / a0..a5
   │   (SDL built-in "PSVita Controller" gamecontroller mapping — FIXED, not ours)
   ▼
SDL_GameController button / axis             SDL_CONTROLLER_BUTTON_A, _AXIS_LEFTX, …
   │   (libultraship default mappings — ★ OURS, edit here ★)
   ▼
N64 logical button                           BTN_A, BTN_Z, BTN_CUP, LEFT_STICK, …
   │   (libultraship ControlDeck assembles the 16-bit OSContPad.button + stick_x/y)
   ▼
OSContPad handed to the decomp
   │   (decomp/src/sys/controller.c — ★ OURS, Vita gameplay tweaks live here ★)
   ▼
gameplay (SSB64 movement / attacks)
```

Only layers 3 and 4 are ours to change. Layers 1 and 2 are baked into the
VitaSDK-bundled SDL2 (`/usr/local/vitasdk/arm-vita-eabi/lib/libSDL2.a`) and are
documented here only so the chain is traceable.

---

## Layer 1 — physical Vita control → SDL index (FIXED)

VitaSDK's `SDL_sysjoystick.c` (`VITA_Joystick*`) uses a fixed button-order table.
Verified by disassembling the shipped `libSDL2.a` on 2026-08-28.

| SDL button | Physical Vita control        | Notes |
|-----------:|------------------------------|-------|
| b0         | ▲ Triangle                   |       |
| b1         | ● Circle                     |       |
| b2         | ✕ Cross                      |       |
| b3         | ■ Square                     |       |
| b4         | L (top-left shoulder)        | the only left shoulder a phat/slim Vita has |
| b5         | R (top-right shoulder)       | the only right shoulder |
| b6         | D-pad Down                   |       |
| b7         | D-pad Left                   |       |
| b8         | D-pad Up                     |       |
| b9         | D-pad Right                  |       |
| b10        | SELECT                       |       |
| b11        | START                        |       |
| b12        | L1                           | PSTV / DualShock-4-over-PSTV only — never fires on a handheld |
| b13        | R1                           | PSTV only |
| b14        | L3 (left stick click)        | PSTV only |
| b15        | R3 (right stick click)       | PSTV only |

| SDL axis | Physical Vita control | Range |
|---------:|-----------------------|-------|
| a0       | Left stick X          | −32768..32767 |
| a1       | Left stick Y          | −32768..32767 (SDL reports **inverted**, see layer 2 `~`) |
| a2       | Right stick X         | −32768..32767 |
| a3       | Right stick Y         | −32768..32767 (inverted) |
| a4       | L2 analog             | PSTV / DS4 only — stays 0 on a handheld |
| a5       | R2 analog             | PSTV / DS4 only — stays 0 on a handheld |

## Layer 2 — SDL index → SDL_GameController (FIXED)

SDL 2 ships a hard-coded gamecontroller mapping keyed on the name
`PSVita Controller` (in `SDL_gamecontroller.c`'s built-in list — no
`gamecontrollerdb.txt` needed, and `VITA_JoystickGetGamepadMapping` returns
none, so this built-in is what's used):

```
a:b2, b:b1, x:b3, y:b0,
back:b10, start:b11,
leftshoulder:b4, rightshoulder:b5,
leftstick:b14, rightstick:b15,
dpup:b8, dpdown:b6, dpleft:b7, dpright:b9,
lefttrigger:a4, righttrigger:a5,
leftx:a0, lefty:a1~, rightx:a2, righty:a3~
```

Net effect (physical control → SDL_GameController enum):

| SDL_GameController symbol            | Physical Vita control |
|-------------------------------------|-----------------------|
| `SDL_CONTROLLER_BUTTON_A`            | ✕ Cross               |
| `SDL_CONTROLLER_BUTTON_B`            | ● Circle              |
| `SDL_CONTROLLER_BUTTON_X`            | ■ Square              |
| `SDL_CONTROLLER_BUTTON_Y`            | ▲ Triangle            |
| `SDL_CONTROLLER_BUTTON_BACK`         | SELECT                |
| `SDL_CONTROLLER_BUTTON_START`        | START                 |
| `SDL_CONTROLLER_BUTTON_LEFTSHOULDER` | L                     |
| `SDL_CONTROLLER_BUTTON_RIGHTSHOULDER`| R                     |
| `SDL_CONTROLLER_BUTTON_DPAD_UP/DOWN/LEFT/RIGHT` | D-pad      |
| `SDL_CONTROLLER_AXIS_LEFTX / LEFTY`  | Left stick            |
| `SDL_CONTROLLER_AXIS_RIGHTX / RIGHTY`| Right stick           |
| `SDL_CONTROLLER_AXIS_TRIGGERLEFT / TRIGGERRIGHT` | **absent** on a handheld (a4/a5 = 0) |

This is the key constraint the port works around: **a real Vita has no analog
triggers**, so `BTN_R` and `BTN_Z` cannot use the desktop trigger-axis defaults.

---

## Layer 3 — SDL_GameController → N64 logical button (★ OURS ★)

Two places define this, and both matter:

### 3a. Fresh-install defaults

`libultraship/src/libultraship/controller/controldevice/controller/mapping/ControllerDefaultMappings.cpp`

- `SetDefaultSDLButtonToButtonMappings()` — button → button, has a full
  `#ifdef __vita__ / #else / #endif` split (the Vita map is the 3DS-style
  layout, the `#else` is the stock desktop map).
- `SetDefaultSDLAxisDirectionToButtonMappings()` — stick direction → button, has
  an `#ifndef __vita__` guard around the trigger entries; the right-stick →
  C-button entries apply on both platforms.
- Stick-to-stick defaults (`LEFT_STICK` / `RIGHT_STICK`) come from the base class
  `libultraship/src/ship/controller/controldevice/controller/mapping/ControllerDefaultMappings.cpp`
  `SetDefaultSDLAxisDirectionToAxisDirectionMappings()` — not Vita-specific.

### 3b. One-time migration for existing installs

`libultraship/src/ship/controller/controldeck/ControlDeck.cpp`, `#ifdef __vita__`
block.

**Why this exists:** an installed build already has `HasConfig() == true`, so
its saved config (`BattleShip.cfg.json`) wins and changing 3a alone would never
reach existing users. The migration is gated on a version cvar:

```
cvar key:  gControllers.VitaButtonLayoutVersion   (CVAR_PREFIX_CONTROLLERS ".VitaButtonLayoutVersion")
current:   kVitaButtonLayoutVersion = 2
```

When the stored version is behind, for every port it does
`ClearAllMappingsForDeviceType(SDLGamepad)` then
`AddDefaultMappings(SDLGamepad)` — i.e. it wipes the port's **gamepad** button
and stick mappings and re-pulls the current 3a defaults (keyboard/mouse
untouched). Any hand-customized gamepad binding is reset by this one-time pass.
`v2` = the SSB64-3DS-style layout below (`v1` was the earlier trigger-less
patch that only *added* R/Z/C-Up).

### Current effective Vita → N64 mapping (v2, SSB64-3DS-style)

| Vita control  | N64 input     | SSB64 action | Defined in |
|---------------|---------------|--------------|------------|
| ● Circle      | `BTN_A` (A_BUTTON)     | Attack       | 3a button→button (`__vita__`) |
| ✕ Cross       | `BTN_B` (B_BUTTON)     | Special      | 3a button→button (`__vita__`) |
| ▲ Triangle    | `BTN_CUP` (U_CBUTTONS) | Jump         | 3a button→button (`__vita__`) |
| ■ Square      | `BTN_CLEFT` (L_CBUTTONS) | Jump       | 3a button→button (`__vita__`) |
| L shoulder    | `BTN_R` (R_TRIG)      | Grab (SSB64 R_TRIG → A+Z) | 3a button→button (`__vita__`) |
| R shoulder    | `BTN_Z` (Z_TRIG)      | Shield       | 3a button→button (`__vita__`) |
| D-pad (any dir)| `BTN_L` (L_TRIG)     | Taunt / appeal | 3a button→button (`__vita__`) |
| START         | `BTN_START`          | Pause        | 3a button→button (`__vita__`) |
| Left stick    | N64 analog stick     | Move + tap-jump + smash | base-class stick→stick |
| Right stick   | `BTN_CUP/CDOWN/CLEFT/CRIGHT` | menu nav; smashes when `gEnhancements.CStickSmash.P*` is on | 3a axis→button |
| SELECT        | *(unbound)*          | —            | — |

Notes:
- **No D-pad menu navigation** anymore — the D-pad is taunt. Navigate menus
  with either stick (both give all four directions), or Triangle/Square (C-Up /
  C-Left).
- SSB64's only jump inputs are tap-up on the stick and *any* C-button; Triangle
  and Square both map to a C-button, so both jump. Right-stick directions also
  produce C-buttons — that's jump/menu-nav by default, directional smashes once
  the C-Stick Smash enhancement is enabled (it strips the raw C-bits and
  re-synthesizes an A-smash).
- The Vita has no analog triggers (`AXIS_TRIGGERLEFT/RIGHT` stay 0), which is
  why R_TRIG / Z_TRIG live on the shoulder buttons.

---

## Layer 4 — decomp-side Vita gameplay tweaks (★ OURS ★)

`decomp/src/sys/controller.c`, `#if defined(PORT) && defined(__vita__)` blocks.
These run *after* libultraship has produced the N64 button bits + stick values,
inside `syControllerUpdateGlobalData()`.

### D-pad as analog-stick approximation (`syControllerVitaApplyDPadMovement`)

> **Inert under the v2 layout.** This reads the N64 D-pad bits
> (`U_JPAD`/`D_JPAD`/`L_JPAD`/`R_JPAD`), but v2 maps the physical D-pad to
> `L_TRIG` (taunt), so nothing ever sets those bits and this function is a
> no-op. Left in place (not removed) in case the D-pad is ever remapped back.

Only active during gameplay scenes (`nSCKindVSBattle`, `nSCKind1PGame`,
`nSCKind1PBonusStage`, `nSCKind1PTrainingMode`). When the real analog stick axis
is 0 and exactly one D-pad direction is held, it writes a synthetic stick value:

| Constant | Value | Meaning |
|----------|-------|---------|
| `SYCONTROLLER_VITA_DPAD_SLOW_RANGE` | 48 | single-press horizontal — below Smash's dash/run threshold (walk) |
| `SYCONTROLLER_VITA_DPAD_FAST_RANGE` | 80 | double-tap-within-window horizontal, and **all** vertical input |
| `SYCONTROLLER_VITA_DPAD_DOUBLE_TAP_TICS` | 13 | ~220 ms window for the double-tap-to-dash |

Horizontal: tap = walk (48), tap-tap fast = dash (80), released = fast flag
clears. Vertical: always full range (80) so jump/crouch/platform-drop feel
identical to a full stick push. The real stick always wins when both are used.

### Enhancement hooks (also decomp-side, not Vita-only but relevant)

Called from the same loop in `syControllerUpdateGlobalData()`:

| Hook | File | Cvars | What |
|------|------|-------|------|
| `port_enhancement_analog_remap` | `port/enhancements/AnalogRemap.cpp` | `gEnhancements.AnalogRemap.P{1..4}.{Enabled,Deadzone,Range}` | per-axis NRage-style stick deadzone/range shaping; runs everywhere, no-op when disabled |
| `port_enhancement_c_stick_smash` | `port/enhancements/CStickSmash.cpp` | `gEnhancements.CStickSmash.P{1..4}` | right stick → smash attacks; gameplay only |
| `port_enhancement_dpad_jump` | `port/enhancements/DPadJump.cpp` | `gEnhancements.DPadJump.P{1..4}` | D-pad up → jump; gameplay only |

---

## Runtime config on the device

**All settings live in one file:** `ux0:data/battleship/BattleShip.cfg.json`
(libultraship `Config`, one shared JSON — there is no separate `controllers.cfg`).
Key facts:

- It is **pretty-printed nested JSON**. A cvar key with dots
  (`gControllers.Port1.HasConfig`) is stored as nested objects; a dotless cvar
  (`gControlNav`) is a top-level key.
- libultraship **truncates and rewrites the whole file** on every settings change
  and on a clean exit. So **only hand-edit it while the app is not running**, or
  your edit is lost.
- `gControllers.Port{1..4}.HasConfig = 1` means "this port has a saved config" —
  once set, fresh-install defaults (layer 3a) are **not** re-applied. Only the
  layer-3b migration can still inject bindings.
- `gControllers.VitaButtonLayoutVersion` tracks which 3b migration steps have run
  (currently `1`). If it is missing or `< 1`, the next boot re-adds
  `R←RIGHTSHOULDER`, `Z←X`, `CUp←Y` — so if you hand-edit those away, also set
  this to `1` (or higher).
- Full reset: delete `BattleShip.cfg.json` (loses *all* settings, not just
  input), or use the in-game Input Editor's per-port "Set Defaults".

### Path A — in-app Input Editor (status: NOT usable on Vita yet)

The Input Editor GUI *is* compiled into the Vita build, but there is currently
no working way to reach and operate it on a handheld:

- Opening the ImGui menu needs the **SELECT** (`ImGuiKey_GamepadBack`) shortcut,
  which is gated behind `gControlNav` (default off). You can flip that cvar by
  hand-editing `ux0:data/battleship/BattleShip.cfg.json` (top-level
  `"gControlNav": 1`) while the app is closed.
- Even with the menu open, driving the Input Editor with the Vita's buttons was
  tried and did not work in practice. An in-game "CONTROLLER SETUP" menu entry
  (`port_open_controller_config()`) was prototyped and reverted for the same
  reason — the ImGui side is not navigable with the pad on this build.

Until that is fixed, use **Path B** (hand-edit the JSON). Rewiring ImGui gamepad
navigation on Vita — or feeding it Vita input through a different path — is the
open task here.

### Path B — hand-edit the JSON (full control, scriptable)

Do this with the app closed. Two things must agree: the **id list** for the
button, and a **mapping-definition block** for each id in that list.

Button identity:

| N64 button | name | bitmask (dec) |
|------------|------|---------------|
| A | `A` | 32768 |
| B | `B` | 16384 |
| Z | `Z` | 8192 |
| Start | `Start` | 4096 |
| L | `L` | 32 |
| R | `R` | 16 |
| C-Up / C-Down / C-Left / C-Right | `CUp` `CDown` `CLeft` `CRight` | 8 / 4 / 2 / 1 |
| D-Up / D-Down / D-Left / D-Right | `DUp` `DDown` `DLeft` `DRight` | 2048 / 1024 / 512 / 256 |

`SDL_CONTROLLER_BUTTON_*` numeric values (for `SDLControllerButton`):
`A=0 B=1 X=2 Y=3 BACK=4 GUIDE=5 START=6 LEFTSTICK=7 RIGHTSTICK=8 LEFTSHOULDER=9
RIGHTSHOULDER=10 DPAD_UP=11 DPAD_DOWN=12 DPAD_LEFT=13 DPAD_RIGHT=14`.

`SDL_CONTROLLER_AXIS_*` values (for `SDLControllerAxis`):
`LEFTX=0 LEFTY=1 RIGHTX=2 RIGHTY=3 TRIGGERLEFT=4 TRIGGERRIGHT=5`
(4 and 5 never move on a handheld Vita).

Note the **port number is written two ways**: the id string uses a **0-based**
`P0`, the `Port` group uses a **1-based** `Port1`. Both refer to player 1.

Mapping id formats:
- button→button: `P0-B<bitmask>-SDLB<sdlButton>`
- axis-direction→button: `P0-B<bitmask>-SDLA<sdlAxis>-AD<P|N>` (`P` = positive
  direction, `N` = negative)

**Worked example — move N64 `Z` (shield) from R shoulder (SDL `RIGHTSHOULDER`=10,
the v2 default) onto ▲ Triangle (SDL `Y`=3):**

```jsonc
{
  "gControllers": {
    "VitaButtonLayoutVersion": 2,          // keep == current so the migration won't overwrite this
    "Port1": {
      "HasConfig": 1,
      "Buttons": {
        "ZButtonMappingIds": "P0-B8192-SDLB3,"   // was "P0-B8192-SDLB10,"
      }
    },
    "ButtonMappings": {
      "P0-B8192-SDLB3": {
        "ButtonMappingClass": "SDLButtonToButtonMapping",
        "Bitmask": 8192,
        "SDLControllerButton": 3
      }
      // the old "P0-B8192-SDLB10" block can be deleted, or left unused
    }
  }
}
```

The id list is a **comma-terminated** string and may hold several ids
(`"P0-B8-SDLB3,P0-B8-SDLA3-ADN,"` = C-Up on Triangle *and* right-stick-up). Every
id listed must have a matching block under `ButtonMappings`, or it is dropped on
load. **Keep `VitaButtonLayoutVersion` equal to the current
`kVitaButtonLayoutVersion`** — if it is lower, the next boot wipes and
re-defaults every gamepad binding.

For the **analog stick** (left stick → N64 stick) the mappings are
axis-direction→axis-direction and are keyed under `gControllers.Port1.…` with
class `SDLAxisDirectionToAxisDirectionMapping` — easiest to change through the
Input Editor's stick section rather than by hand.

---

## How to change a binding in the future

1. **Decide which layer.**
   - Different Vita button for an existing N64 action, or a new default →
     **layer 3a** (`ControllerDefaultMappings.cpp`), inside the `#ifdef __vita__`
     block. Keep the non-Vita path unchanged.
   - Want existing installs (not just fresh) to pick it up → **bump**
     `kVitaButtonLayoutVersion` in `ControlDeck.cpp` by 1. The migration there
     re-applies the current 3a defaults to every port's gamepad bindings once
     (it clears + re-adds `SDLGamepad`). Without the bump, devices that already
     ran version N keep their old bindings forever.
   - Movement feel / D-pad-as-stick ranges / double-tap timing → **layer 4**
     constants in `decomp/src/sys/controller.c`.
2. **Search for every occurrence** of the N64 logical name you touch:
   `BTN_R`, `BTN_Z`, `BTN_CUP`, `LEFT_STICK`, etc. across both
   `libultraship/src/libultraship/...` and `libultraship/src/ship/...`
   (there are two `ControllerDefaultMappings.cpp` — LUS subclass and Ship base).
3. **decomp submodule changes** follow the worktree/submodule flow in the root
   `CLAUDE.md` ("PS Vita Port" section): commit inside `decomp/`, push to
   `robin994/ssb-decomp-re-vita` `vita-compat-fixes`, bump the pointer.
4. **Rebuild** — a libultraship or macro change may need a clean build:
   ```sh
   make -f Makefile.vita objects -j$(sysctl -n hw.ncpu)
   make -f Makefile.vita prepare        # NOT optional
   make -f Makefile.vita build/battleship.vpk
   ```
   Verify the edit is in the binary:
   `strings build/battleship.elf.unstripped.elf | grep <something unique>`.
5. **Test on hardware.** A fresh install exercises layer 3a; an in-place update
   over an existing install exercises the 3b migration — test both if you
   touched the migration.

### Quick reference: the two files you will almost always edit

| Change | File | Marker |
|--------|------|--------|
| Vita button-to-N64-button default | `libultraship/src/libultraship/controller/controldevice/controller/mapping/ControllerDefaultMappings.cpp` | `#ifdef __vita__` branch of `SetDefaultSDLButtonToButtonMappings` |
| Vita stick-direction-to-N64-button default | same file | `SetDefaultSDLAxisDirectionToButtonMappings` (`#ifndef __vita__` guards the trigger entries) |
| Make it reach already-installed devices | `libultraship/src/ship/controller/controldeck/ControlDeck.cpp` | bump `kVitaButtonLayoutVersion` in the `#ifdef __vita__` migration |
| D-pad movement feel / timing | `decomp/src/sys/controller.c` | `SYCONTROLLER_VITA_DPAD_*` macros |
