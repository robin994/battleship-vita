# Changelog

All notable changes to the **PS Vita** port. Dates are ISO (YYYY-MM-DD).
The other platforms (macOS / Linux / Windows / Android) are unaffected by these
entries unless stated.

---

## 1.3 — 2026-08-30

Headline: **netplay** (local ad-hoc and online), a reworked **controller
layout** modelled on the SSB64 3DS port, and the **Classic (1P) co-op** mode.

### Netplay — local ad-hoc

- Two PS Vitas on the same Wi-Fi (or PS TV) can host/join a lobby, pick
  characters together and fight, using the system's PSP-AdHoc connection
  dialog. No router or internet needed.
- Host **or** client can be P1; up to 4 human players across consoles is
  wired but only 1v1 / 2-player is well tested.

### Netplay — online (over the internet)

- **Direct IP**: the host opens ports on their router, the joiner types the
  host's public IP. Deterministic lockstep with **predictive rollback**
  (6-frame window) so a normal home-broadband ping stays playable.
- **Automatic port forwarding (UPnP)**: when you host an online game the port
  tries to open `26041/tcp` + `26042/udp` on your router by itself and shows
  your public IP right in the lobby. Works only if your router has UPnP
  enabled; falls back to "forward these ports manually".
- **Lobby board (optional, self-hosted)**: a tiny always-on server
  (`server/matchmaker/`, ~single Go binary, runs on a Raspberry Pi / NAS /
  old PC with one forwarded TCP port) lists open lobbies so joiners can
  **FIND GAME** without knowing anyone's IP. Gameplay is still direct
  peer-to-peer — the board only advertises. No accounts, no game data stored.
- **Graceful desync recovery**: if the two consoles' simulations diverge, both
  drop back to the host lobby with a "DESYNC — RETURNED TO LOBBY" message
  instead of a dead disconnect screen.
- See **[Playing online](README.md#playing-online-ps-vita)** in the README for
  the step-by-step.

### Host match rules

- A dedicated **MATCH RULES** page (host presses **R** in the lobby): stage,
  lives, time, **items** (frequency + a per-item on/off list of all 15 item
  types), **team battle**, **friendly fire**, **damage ratio** (50–200 %),
  **handicap** (off / on / auto). RANDOM is selectable for stage, lives, time,
  item rate and damage. Clients see the full rule set before readying up.
- Fixed: a 2-player **team battle** never ended — both players landed on the
  same team, so the match had no losing side. Teams are now auto-alternated.

### Controls (PS Vita)

Layout modelled on the official SSB64 3DS port. On the Vita (no analog
triggers):

| Physical | N64 |
|---|---|
| Circle | A — attack |
| Cross | B — special |
| Triangle / Square | C-Up / C-Left jump |
| L | R-Trigger — grab |
| R | Z-Trigger — shield |
| **D-pad** | **N64 D-pad** — menu navigation everywhere; in gameplay it emulates the left stick (up/down = jump/crouch, left/right = walk on a tap, dash on a double-tap) |
| **Right analog stick** | **taunt** (flick it, in gameplay) |
| Start | pause |
| Left stick / Right stick | N64 stick / C-buttons |

- The D-pad and right-stick behaviour is forced from `sceCtrl` directly, so it
  applies even if you have an old saved controller-binding config.
- The in-game controller editor is disabled on Vita; remap by hand in
  `ux0:data/battleship/BattleShip.cfg.json` (see the README "Controls (PS Vita)"
  section).

### Classic / 1P Mode

- **Co-op**: a second human (or a CPU) can play the Classic 1P arcade run
  alongside P1 — CSS slot lock, side-by-side spawns, per-stage setup, bonus
  stages, co-op death / continue rules, 2-player challenger duels, no friendly
  fire.
- Classic mode is now served through the VS character-select screen.
- Scene-manager fix so `1P Game` / challenger / Bonus stages report the right
  scene id (the intro/challenger code used to clobber it to `Title`), which
  the port relies on to gate input enhancements to actual gameplay.
- In-game **reset** support for the port's console reset command.

### Menus

- **MULTIPLAYER** is no longer a top-level Mode-Select row — it's a **NETPLAY**
  entry inside the **VS MODE** menu.
- The whole netplay UI was restyled to match VS MODE / VS OPTIONS (tab widgets,
  kerned text, the shared collage background, the gold cursor).
- Text entry (player name, direct IP, lobby-server address) uses the **PS Vita
  system keyboard** instead of a scroll-wheel character picker.

### Rendering & stability

- Continued Fast3D / vitaGL performance work toward a stable 60 FPS
  (texture-fixup path, shader pre-warm, param-buffer sizing).
- Boot-time crash fixes: `SceShaccCg` shader-compiler heap starvation, frame
  presentation, stale texture-cache comparison, intro-scene scale-seed OOB,
  stack/heap sizing, buffered-stdio hang in the logger, `.o2r` archive read
  path on real firmware.
- Runtime diagnostics are now behind `PORT_RUNTIME_DIAGNOSTICS` and quieter by
  default.

### Notes / known gaps

- `%z`/`%zu` printf length modifier is unusable on this VitaSDK toolchain —
  avoid it in any logging / `ImGui::Text`.
- 3–4 player netplay is not verified end-to-end.
- Behind CGNAT (some fibre / mobile), a console can only **join**, not host —
  neither UPnP nor manual forwarding helps; a relay is not yet implemented.
- Real-hardware behaviour can diverge from the Vita3K emulator; test on device.

---

## 1.1 and earlier

See the git history and `docs/` for the earlier Vita bring-up (video/audio on
real hardware, boot-bug write-ups in `docs/bugs/`).
