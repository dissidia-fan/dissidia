#pragma once
// PSP ad-hoc networking emulation (sceNetAdhoc*, sceNetAdhocctl*, sceNetAdhocMatching*).
//
// Transport: UDP. Instances find each other with broadcast beacons on the LAN (and on
// 127.0.0.1 for several instances on one machine), or by direct address through
// PSPRECOMP_ADHOC_PEERS for internet play (forward the UDP port or use a VPN).
//
// Environment:
//   PSPRECOMP_ADHOC=0            turn the ad-hoc emulation off (the old no-op stubs remain)
//   PSPRECOMP_ADHOC_MODE=lan|internet  how players find each other (default internet; lan when
//                                 PSPRECOMP_ADHOC_PEERS is set and no mode is given)
//   PSPRECOMP_ADHOC_SERVER=h[:p] the relay server for Internet mode (default party.schindlershadow.com:27500)
//   PSPRECOMP_ADHOC_ROOM=<name>  room on the server; players in one room see each other (default: the public room)
//   PSPRECOMP_ADHOC_PORT=<n>     first UDP port to try (default 27312; tries n..n+7)
//   PSPRECOMP_ADHOC_PEERS=a,b    LAN mode: extra peers to contact directly: host or host:port, comma separated
//   PSPRECOMP_ADHOC_NICK=<name>  nickname shown to other players (default "Player")
//   PSPRECOMP_ADHOC_MAC=aa:bb:.. override the virtual MAC (default: random per launch)
//   PSPRECOMP_ADHOC_LOG=1        log every packet-level event
#include <cstddef>
#include <cstdint>

struct recomp_context;

void psp_hle_register_adhoc();

/// Run queued guest callbacks (adhocctl handlers, matching callbacks) on the calling guest
/// thread. Safe to call often; does nothing when the queue is empty or a dispatch is running.
void psp_adhoc_pump(uint8_t* rdram, recomp_context* ctx);

/// The frame rate (30 or 60) this instance would like. 0 = unknown. Sent to other players.
void psp_adhoc_set_local_fps(int fps);

/// The frame rate the whole connected group should run at: the lowest of every player's
/// preference, or 0 when not in a group with anyone else (no limit).
int psp_adhoc_group_fps_cap();

/// True while this instance is in an ad-hoc group with at least one other player.
bool psp_adhoc_in_session();

/// Change the network setup at run time (the settings menu). `internet` selects relay-server mode;
/// `server`/`room` apply to it, `peers` (comma separated) to LAN mode. Takes effect within a few
/// milliseconds; players found so far are forgotten.
void psp_adhoc_configure(bool internet, const char* server, const char* room, const char* peers);

/// A one-line description of the network state for the settings menu.
void psp_adhoc_status(char* out, size_t n);
