# Multiplayer (ad-hoc wireless over UDP)

Dissidia 012's "Communications" mode was written for two or more PSPs talking over ad-hoc WiFi. The native build
emulates that layer on top of UDP, so players on the same network, on one machine, or across the internet can play
each other. It is **experimental**: a full match has been played between two instances on one machine, and nothing
has been tested with more than two players, with real WiFi/LAN hardware between machines, or across the internet.

The code is generic runtime code (`runtime/src/hle/psp_hle_adhoc.cpp`), not Dissidia-specific. The only game-specific
part is the frame-rate rule in `runtime/framerate.cpp`.

## Playing

Open Communications mode in the menu and challenge a player as on a PSP. Nothing else needs turning on: the network
layer starts the first time the game uses it. Players need the same game version and character set (do not mix a
modded roster with an unmodded one). Both players need a free, unmodified settings page, or the same choices below.

### Internet (the default)

By default the game connects to a **party server**, `party.schindlershadow.com`, which relays the game traffic
between players. Nobody has to forward a port or know anyone's address.

- Players in the same **room** see each other in the lobby. The default is the public room, where everyone using the
  server is. To play with friends only, all of you set the same room name (any name only you know).
- The settings page is **Settings (Esc / F1 / Guide) > Network** (L / R shoulder, Q / E or Tab switch pages):

  | Row | Meaning |
  |---|---|
  | Network | **Internet** (relay server, default) or **LAN** |
  | Server | the relay: a host name or `host:port` (default `party.schindlershadow.com:27500`); Cross edits |
  | Room | blank = the public room; Cross edits |
  | Direct Peers | LAN only: extra addresses to contact directly; Cross edits |
  | Status | `Connected, 3 online`, `No answer from the server yet`, `Cannot find ...`, `LAN: 2 nearby` |

  Editing text: type on a keyboard (Enter saves, Esc cancels), or with a controller pick a letter with left / right,
  add it with Cross, delete with Circle, and save with Start or the Done row. Changes apply at once and are saved
  in `settings.ini` (`net_mode`, `net_server`, `net_room`, `net_peers`).
- Everything you send goes through the server, and the public room shows your player name to whoever is in it. The
  server never looks inside a game message and does not log them, but it has no accounts and no encryption: use a
  private room name for friends.

### LAN

Set Network to **LAN**. Players on the same network find each other by broadcast and show up in the lobby by name
(some routers and Wi-Fi isolation block broadcast; list the other player under Direct Peers then). No server is
involved, so it has the lowest latency.

Two copies on one machine (for testing) need LAN mode and separate work folders, because the game saves into its
folder:

```
cp -a ~/dissidia/work ~/dissidia/work2        # or symlink everything but ms0/, settings.ini
PSPRECOMP_ADHOC_MODE=lan PSPRECOMP_ADHOC_NICK=A tools/run.sh ~/dissidia/work  --window 960x544 &
PSPRECOMP_ADHOC_MODE=lan PSPRECOMP_ADHOC_NICK=B PSPRECOMP_DEBUG_PORT=10000 tools/run.sh ~/dissidia/work2 --window 960x544 &
```

The copies find each other over `127.0.0.1`: each takes the first free UDP port from 27312. Controllers only act on the
window that has focus (set `PSPRECOMP_PAD_BACKGROUND=1` to listen while unfocused). To test through a relay instead,
use `PSPRECOMP_ADHOC_MODE=internet PSPRECOMP_ADHOC_SERVER=127.0.0.1:27500`.

### Direct internet play without the server

In LAN mode a player can also be given an address directly (`PSPRECOMP_ADHOC_PEERS=friend.example.org`, or Direct
Peers in the menu). That needs UDP port 27312 reachable on both sides (forward it, or use a VPN such as ZeroTier,
Tailscale or WireGuard). Both players should list each other; a third and later player needs only one address,
because beacons carry the addresses of the peers each player hears (unknown addresses are probed for 15 s and dropped
if they do not answer).

**Expect latency to matter.** The game runs a lock-step battle over reliable streams, and nothing here delays or
predicts input. It was built for a few milliseconds of WiFi latency. A relay adds the trip to the server on top of the
direct path; how much round-trip time the game tolerates has not been measured, so try a nearby partner first.

## Frame rate

Every player's wanted frame rate (30 or 60, from the settings menu) is shared in the beacons. While a player is in a
group with others, the game runs at the **lowest rate any of them asked for**, so a 60 fps player and a 30 fps player
both run at 30. It returns to the player's own setting when the group ends. This logic is in
`runtime/framerate.cpp` (`psp_adhoc_group_fps_cap()`).

## Options

| Variable | Meaning |
|---|---|
| `PSPRECOMP_ADHOC=0` | turn the emulation off (the old no-op stubs return, Communications mode then cannot connect) |
| `PSPRECOMP_ADHOC_MODE=lan\|internet` | how players find each other; default `internet` (`lan` if `PSPRECOMP_ADHOC_PEERS` is set and no mode is given). Saved settings win |
| `PSPRECOMP_ADHOC_SERVER=host[:port]` | the relay for Internet mode (default `party.schindlershadow.com:27500`) |
| `PSPRECOMP_ADHOC_ROOM=<name>` | room on the relay; empty = public |
| `PSPRECOMP_ADHOC_PORT=<n>` | LAN mode: first UDP port (default 27312) |
| `PSPRECOMP_ADHOC_SPAN=<n>` | how many consecutive ports are tried and probed (default 16, up to 64) |
| `PSPRECOMP_ADHOC_PEERS=a,b` | LAN mode: extra peers to contact directly: `host` or `host:port` (a bare host probes the whole port span) |
| `PSPRECOMP_ADHOC_NICK=<name>` | nickname other adhocctl clients see (the in-game player name comes from the save) |
| `PSPRECOMP_ADHOC_MAC=aa:bb:cc:dd:ee:ff` | virtual MAC; default is random per launch (a host-name hash collides: every Steam Deck is called `steamdeck`) |
| `PSPRECOMP_ADHOC_LOG=1` | log every call and event with arguments, results and the first bytes of each message |
| `PSPRECOMP_PAD_BACKGROUND=1` | let controllers act while the window is unfocused |

Log lines start with `[ADHOC]` in `last_run.log`.

## How it works

The game uses four layers of the PSP network API; each is emulated:

| Layer | Calls | Emulation |
|---|---|---|
| Wlan / net helpers | `sceWlanGetEtherAddr`, `sceNetGetLocalEtherAddr`, `sceNetEtherNtostr` | a virtual MAC per instance |
| Ad-hoc control (adhocctl) | `Init/Term/AddHandler/DelHandler`, `Create/Connect/Join/Disconnect/Scan`, `GetScanInfo/GetPeerList/GetPeerInfo` | a "group" is a name; peers advertise their group in beacons. Events (connect, disconnect, scan) are queued and delivered to the game's handlers |
| Matching | `MatchingInit/Create/Start/Stop/Delete`, `SelectTarget/CancelTarget/SendData/SetHelloOpt` | the lobby state machine: hello, request, accept, established, leave, cancel, data and ping/timeout, as events on the game's callback |
| PDP / PTP | `PdpCreate/Send/Recv`, `PtpListen/Open/Connect/Accept/Send/Recv/Flush/Close` | PDP is a datagram per call; PTP is a TCP-like reliable stream (connection ids, go-back-N retransmission, partial sends) |

Transport: one UDP socket and one network thread. In LAN mode the socket takes the first free port from 27312 and sends to the LAN, loopback and listed peers; in Internet mode every packet goes to the relay server. Packets start with `DSAH`, a type, the sender's MAC and a body
length. Types: beacon (nickname, group, wanted frame rate, known peers), PDP, matching, PTP. Beacons go out twice a
second to the local ports, the LAN broadcast address and any listed peers; a peer is dropped after 4 s of silence.

Guest callbacks never run on the network thread. They are queued and run on a game thread, from
`sceKernelCheckCallback` and the vblank wait (`psp_adhoc_pump`), because they call back into recompiled code.

Details worth knowing when changing it:

- The game checks every peer with `sceNetAdhocctlGetPeerInfo` and drops anyone whose `last_recv` (game clock) is older
  than 20 s, so the field is filled from the beacon time.
- After the PTP handshake the game keeps opening new sockets on the same ports whenever a connect fails. A SYN with no
  listener must be refused (RST) so it retries, and every packet carries a connection id so replies reach the right
  socket.
- `sceNetAdhocPtpSend` is `send()`-like: it queues what fits in the socket's buffer and writes the count into `*size`
  (0 on would-block). The game adds that count to what it has sent.
- Struct layouts follow PPSSPP: scan info 28 bytes, peer info 152 bytes, PDP stat 20, PTP stat 36 (the last two are
  unchecked against the game).

## The party server

`tools/adhoc_relay/` ([README](../../../../tools/adhoc_relay/README.md)) is a small C++ UDP relay. A client registers into a
room with a cookie handshake (so a forged source address cannot make the server flood someone), then the server
copies its beacons to the rest of the room and its PDP / matching / PTP packets to the player whose MAC is the
destination (everyone else for the broadcast MAC). Packets are forwarded untouched; the client sends one copy and
the server fans out, so a 10-player room costs each player the same upstream as two. Per-client rate limits and a
32-player room size protect it. The client keeps the registration alive with a hello every second and treats 5 s of
silence as a lost server. `party.schindlershadow.com` runs it as a user service on a home server, with UDP 27500
forwarded; run your own with the README's two lines and point Server at it.

## Not implemented

- `sceNetAdhocDiscover*` and game mode (`sceNetAdhocctlCreateEnterGameMode` and friends): the game imports Discover but
  nothing in Communications mode was seen to need it.
- Input delay or rollback for high latency, and any form of authentication or encryption. A single relay host is a
  single point of failure.
- Infrastructure (WiFi access point) mode: the game does not use it.
