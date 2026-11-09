#pragma once
// PSP ad-hoc networking emulation (sceNetAdhoc*, sceNetAdhocctl*, sceNetAdhocMatching*).
//
// Transport: UDP. Instances find each other with broadcast beacons on the LAN (and on
// 127.0.0.1 for several instances on one machine), or by direct address through
// PSPRECOMP_ADHOC_PEERS for internet play (forward the UDP port or use a VPN).
//
// Environment:
//   PSPRECOMP_ADHOC=0            turn the ad-hoc emulation off (the old no-op stubs remain)
//   PSPRECOMP_ADHOC_PORT=<n>     first UDP port to try (default 27312; tries n..n+7)
//   PSPRECOMP_ADHOC_PEERS=a,b    extra peers: host or host:port, comma separated
//   PSPRECOMP_ADHOC_NICK=<name>  nickname shown to other players (default "Player")
//   PSPRECOMP_ADHOC_MAC=aa:bb:.. override the virtual MAC (default derived from host + port)
//   PSPRECOMP_ADHOC_LOG=1        log every packet-level event
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
