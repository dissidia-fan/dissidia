# adhoc_relay

UDP relay for the native build's ad-hoc multiplayer. Players behind NAT send all their game traffic here instead of to each other, so nobody has to forward a port. The default server is `party.schindlershadow.com:27500`. Protocol and usage: [games/dissidia/docs/multiplayer.md](../../games/dissidia/docs/multiplayer.md).

```
g++ -O2 -std=c++17 -o adhoc_relay adhoc_relay.cpp
./adhoc_relay --port 27500                 # UDP; forward this port to the machine
python3 test_relay.py 127.0.0.1 27500      # protocol test against a running relay
```

Options: `--bind IP`, `--max-room N` (32), `--max-clients N` (2000), `--pps N` and `--kbps N` (per-client rate limits, 6000 packets/s and 32000 kbit/s), `--verbose`.

How it behaves:

- A client says hello with a room name (empty = the public room) and answers a cookie challenge, so a forged source address cannot register and the server cannot be used to flood a third party.
- Packets are forwarded untouched: a beacon goes to everyone else in the room, anything else to the client whose MAC is the destination (or to the whole room for the broadcast MAC). A packet whose source MAC is not the one registered for that address is dropped, and nothing crosses between rooms.
- A client that is silent for 8 s is removed. A room holds up to 32 players.
- It logs joins and leaves, and a one-line summary every minute. It does not log message contents, and it has no authentication or encryption: a room name is the only secret.

Run it as a service: `adhoc-relay.service` is a systemd user unit (`cp` it to `~/.config/systemd/user/`, put the binary at `~/.local/lib/adhoc_relay/adhoc_relay`, `systemctl --user enable --now adhoc-relay`, `loginctl enable-linger $USER`). Forward UDP 27500 on the router to the host.
