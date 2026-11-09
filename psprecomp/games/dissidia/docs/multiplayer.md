# Multiplayer (ad-hoc wireless over UDP)

Dissidia 012's "Communications" mode was written for two or more PSPs talking over ad-hoc WiFi. The native build
emulates that layer on top of UDP, so players on the same network, on one machine, or across the internet can play
each other. It is **experimental**: a full match has been played between two instances on one machine, and nothing
has been tested with more than two players, with real WiFi/LAN hardware between machines, or across the internet.

The code is generic runtime code (`runtime/src/hle/psp_hle_adhoc.cpp`), not Dissidia-specific. The only game-specific
part is the frame-rate rule in `runtime/framerate.cpp`.

## Playing

1. Every player runs the game normally. Nothing needs to be turned on: the network layer starts the first time the
   game uses it.
2. Open Communications mode in the menu. Players on the same LAN (or the same machine) show up in the lobby by name.
   Challenge one, accept, pick characters and start the battle as on a PSP.

Players need the same game version (US and EU discs are not mixed in testing) and the same character set (do not mix
a modded roster with an unmodded one).

### Two copies on one machine (testing)

Each copy needs its own work folder, because the game saves into it:

```
cp -a ~/dissidia/work ~/dissidia/work2        # or symlink everything but ms0/, settings.ini
PSPRECOMP_ADHOC_NICK=A tools/run.sh ~/dissidia/work  --window 960x544 &
PSPRECOMP_ADHOC_NICK=B PSPRECOMP_DEBUG_PORT=10000 tools/run.sh ~/dissidia/work2 --window 960x544 &
```

The copies find each other over `127.0.0.1`: each takes the first free UDP port from 27312. Controllers only act on the
window that has focus (set `PSPRECOMP_PAD_BACKGROUND=1` to listen while unfocused).

### Over the internet

The network layer is plain UDP; there is no server. Two things have to be true: your packets reach the other player's
UDP port, and each side knows the other's address.

1. **Make the machines reachable.** The simplest way is a VPN that makes everyone look like one LAN (ZeroTier,
   Tailscale, WireGuard): nothing else to configure. Without a VPN, each player forwards UDP port 27312 (and the
   following ports if several copies run on one machine) on their router to their PC. Some routers pass UDP in both
   directions once both sides send to each other (NAT hole punching), so forwarding is not always needed; a
   symmetric NAT or carrier-grade NAT blocks it.
2. **Tell the game where the others are.** Broadcast does not cross the internet (and Tailscale does not forward it),
   so list at least one other player's address:

   ```
   PSPRECOMP_ADHOC_PEERS=friend.example.org run.sh ~/dissidia/work      # host name or IP, port optional
   PSPRECOMP_ADHOC_PEERS=203.0.113.7:27312,198.51.100.9 run.sh ...      # several, comma separated
   ```

   Both players should list each other, so each side sends first and the NATs open. A third and later player only
   needs one address: beacons carry the addresses of the peers each player hears, so everyone learns everyone
   (unknown addresses are probed for 15 s and dropped if they do not answer).
3. Open Communications mode and play as above.

**Expect latency to matter.** The game runs a lock-step battle over reliable streams, and nothing here delays or
predicts input. It was built for a few milliseconds of WiFi latency. How much round-trip time it tolerates, and
whether it buffers input, has not been measured; test with a low-latency partner first. There is no relay, so players
behind a NAT that blocks UDP hole punching need a VPN. There is also **no authentication or encryption**: anyone who
can send UDP to your port can appear in your lobby, so use a VPN or only list people you trust.

## Frame rate

Every player's wanted frame rate (30 or 60, from the settings menu) is shared in the beacons. While a player is in a
group with others, the game runs at the **lowest rate any of them asked for**, so a 60 fps player and a 30 fps player
both run at 30. It returns to the player's own setting when the group ends. This logic is in
`runtime/framerate.cpp` (`psp_adhoc_group_fps_cap()`).

## Options

| Variable | Meaning |
|---|---|
| `PSPRECOMP_ADHOC=0` | turn the emulation off (the old no-op stubs return, Communications mode then cannot connect) |
| `PSPRECOMP_ADHOC_PORT=<n>` | first UDP port (default 27312) |
| `PSPRECOMP_ADHOC_SPAN=<n>` | how many consecutive ports are tried and probed (default 16, up to 64) |
| `PSPRECOMP_ADHOC_PEERS=a,b` | extra peers to contact: `host` or `host:port` (a bare host probes the whole port span) |
| `PSPRECOMP_ADHOC_NICK=<name>` | nickname other adhocctl clients see (the in-game player name comes from the save) |
| `PSPRECOMP_ADHOC_MAC=aa:bb:cc:dd:ee:ff` | virtual MAC; default is derived from host name and port |
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

Transport: one UDP socket and one network thread. Packets start with `DSAH`, a type, the sender's MAC and a body
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

## Not implemented

- `sceNetAdhocDiscover*` and game mode (`sceNetAdhocctlCreateEnterGameMode` and friends): the game imports Discover but
  nothing in Communications mode was seen to need it.
- A relay for players who cannot reach each other directly, input delay or rollback for high latency, and any form of
  authentication or encryption.
- Infrastructure (WiFi access point) mode: the game does not use it.
