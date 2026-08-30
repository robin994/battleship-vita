# SSB64 Vita netplay lobby board

A tiny stateless "notice board" for the Vita port's ONLINE matchmaking. It is
**not** a game server and **not** a relay:

- The host console connects (`REGISTER`), the server records
  `{lobbyId, its public IP as seen on the socket, lobby/gameplay ports, host
  name, player count}` and keeps the TCP connection open as a keepalive.
- Other consoles connect (`LIST`) to get the open lobbies, then connect
  **directly** to the host's public IP — normal BSNP lobby TCP + gameplay UDP,
  exactly like the phase-4 DIRECT-IP path.

No game state, no persistence, ~1 KB per lobby. Lose the board and matchmaking is
down; DIRECT-IP and LAN still work.

**Consequence of the peer-to-peer model:** the host console's router must forward
**26041/tcp** and **26042/udp** to that console. A console behind CGNAT can only
*join*, not *host*. (This matches smash64.online's "P2P mode".)

## Where to run it

Anything always-on that you control: a Raspberry Pi (~2 W), a NAS, an old
laptop, a mini-PC. It needs **one** forwarded TCP port (26050) and a stable
name — use a free dynamic-DNS provider (DuckDNS, No-IP, FreeDNS) since your home
IP changes. The Vita client resolves that hostname.

## Build

On the box (a Pi is arm64/armv7), or cross-compiled from a Mac:

```sh
cd server/matchmaker
# 64-bit Pi / most SBCs:
GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -trimpath -o matchmaker .
# 32-bit Pi (Pi Zero / Pi 1 / older Raspberry Pi OS):
GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0 go build -trimpath -o matchmaker .
scp matchmaker user@<box>:/tmp/
```

Then on the box:

```sh
sudo mkdir -p /opt/matchmaker
sudo mv /tmp/matchmaker /opt/matchmaker/
sudo chmod +x /opt/matchmaker/matchmaker
```

## Firewall + router

```sh
sh server/matchmaker/deploy/firewall.sh
```

Then on your router: forward **TCP 26050** to the box's LAN IP. Set up dynamic
DNS so `something.duckdns.org` always points at your home IP.

## Run under systemd

```sh
sudo cp server/matchmaker/deploy/matchmaker.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now matchmaker
sudo systemctl status matchmaker
journalctl -u matchmaker -f
```

Check: `ss -tlnp | grep 26050`.

## Flags

| flag | default | meaning |
|------|---------|---------|
| `-tcp` | `:26050` | TCP listen address |
| `-build` | `1.3` | required client build id (`BATTLESHIP_CURRENT_VERSION`) |
| `-token` | `""` | optional shared token; if set, the Vita build must carry the same `kRendezvousToken`. Empty = open. |

## Client config

On each Vita: **ONLINE → SETTINGS → SET SERVER** → enter your dynamic-DNS
hostname (e.g. `myboard.duckdns.org`). Port is fixed (26050).

## Wire protocol (for reference)

Framing: `u32 length(BE)` · `u8 op` · body. Strings: `u16 len(BE)` + bytes (≤64).

| op | dir | body |
|----|-----|------|
| `0x01 LIST` | client→server | `str token, str build` → server streams `0x81 ENTRY` then `0x82 END`, closes |
| `0x02 REGISTER` | host→server | `str token, str build, str hostName, u8 maxPlayers, u16 lobbyPort, u16 gameplayPort, u32 bsnpSessionId` → `0x83 REGISTERED{u32 lobbyId}`, conn stays open |
| `0x04 UPDATE` | host→server (open conn) | `u8 players, u8 status` |
| `0x05 PING` / `0x06 PONG` | open conn | host every 5 s; server drops the lobby after 20 s of silence |
| `0x81 ENTRY` | server→client | `u32 lobbyId, str publicIp, u16 lobbyPort, u16 gameplayPort, u32 bsnpSessionId, str hostName, u8 players, u8 max, u8 status, str build` |
