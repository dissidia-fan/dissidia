// adhoc_relay — UDP relay for the native build's ad-hoc multiplayer (see
// runtime/src/hle/psp_hle_adhoc.cpp and games/dissidia/docs/multiplayer.md).
//
// Players that cannot reach each other directly (NAT, no port forwarding) send everything to this
// server instead. A client registers into a *room* with a cookie handshake; the server then
//   * copies a client's beacon to every other client in its room, and
//   * copies PDP / matching / PTP packets to the client whose MAC is the packet's destination,
//     or to everyone else in the room when the destination is the broadcast MAC.
// Datagrams are forwarded untouched. The server never looks inside a game message.
//
// Wire format (shared with the client): "DSAH", type(1), source MAC(6), body length(2, LE), body.
// Types 1-4 and 6 are the client-to-client packets (6 = a fragment of a large message: the client splits what
// does not fit one datagram, because IP fragments are dropped by many routers; the destination MAC leads its body
// like a PDP packet); type 5 is client <-> server:
//   client -> server  op 1 HELLO: room[32], cookie(8)      (cookie 0 asks for one)
//                     op 4 BYE
//   server -> client  op 2 CHALLENGE: cookie(8)
//                     op 3 OK: players in the room(2, LE), version(1)
// The cookie is SipHash(secret, client address, 30 s time bucket). A spoofed source address can
// not complete the handshake, so the server cannot be used to flood a third party.
//
// Build: g++ -O2 -std=c++17 -o adhoc_relay adhoc_relay.cpp
// Run:   ./adhoc_relay [--port 27500] [--bind 0.0.0.0] [--max-room 32] [--max-clients 2000]
//                      [--pps 6000] [--kbps 32000] [--verbose]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Mac = std::array<uint8_t, 6>;
constexpr size_t HDR = 13;
constexpr uint8_t T_BEACON = 1, T_PDP = 2, T_MATCH = 3, T_PTP = 4, T_RELAY = 5, T_FRAG = 6;
constexpr uint8_t OP_HELLO = 1, OP_CHALLENGE = 2, OP_OK = 3, OP_BYE = 4;
constexpr uint8_t VERSION = 1;
constexpr size_t MAX_DATAGRAM = 16384;
constexpr int64_t CLIENT_TIMEOUT_US = 8000000;
constexpr int64_t COOKIE_BUCKET_US = 30000000;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- SipHash-2-4 (cookie) ----
inline uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
uint64_t siphash24(const uint8_t key[16], const uint8_t* in, size_t n) {
    uint64_t k0, k1;
    std::memcpy(&k0, key, 8); std::memcpy(&k1, key + 8, 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t m; std::memcpy(&m, in + i, 8);
        v3 ^= m; round(); round(); v0 ^= m;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t j = 0; i + j < n; j++) b |= (uint64_t)in[i + j] << (8 * j);
    v3 ^= b; round(); round(); v0 ^= b;
    v2 ^= 0xff; round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

uint8_t g_key[16];
uint64_t cookie_for(const sockaddr_in& a, int64_t bucket) {
    uint8_t m[16];
    std::memcpy(m, &a.sin_addr.s_addr, 4);
    std::memcpy(m + 4, &a.sin_port, 2);
    std::memset(m + 6, 0, 2);
    std::memcpy(m + 8, &bucket, 8);
    return siphash24(g_key, m, sizeof m);
}
bool cookie_ok(const sockaddr_in& a, uint64_t c) {
    const int64_t b = now_us() / COOKIE_BUCKET_US;
    return c != 0 && (c == cookie_for(a, b) || c == cookie_for(a, b - 1));
}

uint64_t addr_key(const sockaddr_in& a) { return ((uint64_t)ntohl(a.sin_addr.s_addr) << 16) | ntohs(a.sin_port); }
std::string addr_str(const sockaddr_in& a) {
    char b[64];
    std::snprintf(b, sizeof b, "%s:%u", inet_ntoa(a.sin_addr), ntohs(a.sin_port));
    return b;
}
std::string mac_str(const Mac& m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// ---- configuration ----
struct Config {
    uint16_t port = 27500;
    std::string bind = "0.0.0.0";
    size_t max_room = 32, max_clients = 2000;
    double pps = 6000;          // sustained packets / s per client
    double kbps = 32000;        // sustained kilobit / s per client
    bool verbose = false;
} cfg;

struct Client {
    sockaddr_in addr{};
    Mac mac{};
    std::string room;
    int64_t last_seen = 0;
    // token buckets
    double pkt_tokens = 0, byte_tokens = 0;
    int64_t refill_at = 0;
    uint64_t rx = 0, fwd = 0, dropped = 0;
};

std::unordered_map<uint64_t, std::unique_ptr<Client>> g_clients;                 // by address
std::unordered_map<std::string, std::map<Mac, Client*>> g_rooms;                 // room -> MAC -> client
int g_sock = -1;
volatile sig_atomic_t g_stop = 0;
uint64_t g_total_rx = 0, g_total_fwd = 0, g_total_drop = 0;

void send_to(const sockaddr_in& to, const uint8_t* d, size_t n) {
    (void)!sendto(g_sock, d, n, 0, (const sockaddr*)&to, sizeof to);
}

void send_relay(const sockaddr_in& to, uint8_t op, const uint8_t* payload, size_t n) {
    uint8_t p[HDR + 1 + 16];
    std::memcpy(p, "DSAH", 4);
    p[4] = T_RELAY;
    std::memset(p + 5, 0, 6);
    uint16_t bl = (uint16_t)(1 + n);
    std::memcpy(p + 11, &bl, 2);
    p[HDR] = op;
    if (n) std::memcpy(p + HDR + 1, payload, n);
    send_to(to, p, HDR + 1 + n);
}

void drop_client(uint64_t key, const char* why) {
    auto it = g_clients.find(key);
    if (it == g_clients.end()) return;
    Client* c = it->second.get();
    auto rit = g_rooms.find(c->room);
    if (rit != g_rooms.end()) {
        auto mit = rit->second.find(c->mac);
        if (mit != rit->second.end() && mit->second == c) rit->second.erase(mit);
        if (rit->second.empty()) g_rooms.erase(rit);
    }
    std::printf("- %s %s left room \"%s\" (%s; rx %llu fwd %llu drop %llu)\n", addr_str(c->addr).c_str(),
                mac_str(c->mac).c_str(), c->room.c_str(), why, (unsigned long long)c->rx, (unsigned long long)c->fwd,
                (unsigned long long)c->dropped);
    g_clients.erase(it);
}

bool valid_room(const std::string& r) {
    if (r.size() > 31) return false;
    for (unsigned char ch : r) if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

void handle_hello(const sockaddr_in& from, const Mac& mac, const uint8_t* body, size_t len) {
    if (len < 1 + 32 + 8) return;
    char room_raw[33] = {0};
    std::memcpy(room_raw, body + 1, 32);
    std::string room(room_raw);
    uint64_t cookie; std::memcpy(&cookie, body + 33, 8);
    if (!valid_room(room)) return;
    if (!cookie_ok(from, cookie)) {
        uint64_t c = cookie_for(from, now_us() / COOKIE_BUCKET_US);
        send_relay(from, OP_CHALLENGE, (const uint8_t*)&c, 8);
        return;
    }
    const uint64_t key = addr_key(from);
    const int64_t t = now_us();
    auto it = g_clients.find(key);
    if (it != g_clients.end()) {
        Client* c = it->second.get();
        if (c->mac == mac && c->room == room) {   // keep-alive
            c->last_seen = t;
        } else {
            drop_client(key, "re-registered");
            it = g_clients.end();
        }
    }
    if (it == g_clients.end()) {
        if (g_clients.size() >= cfg.max_clients) return;
        auto& members = g_rooms[room];
        auto old = members.find(mac);
        if (old != members.end()) drop_client(addr_key(old->second->addr), "same player reconnected");
        auto& members2 = g_rooms[room];
        if (members2.size() >= cfg.max_room) {
            if (members2.empty()) g_rooms.erase(room);
            return;   // room full: stay silent, the client keeps retrying and shows "no answer"
        }
        auto c = std::make_unique<Client>();
        c->addr = from; c->mac = mac; c->room = room; c->last_seen = t;
        c->pkt_tokens = cfg.pps * 2; c->byte_tokens = cfg.kbps * 125 * 2; c->refill_at = t;
        members2[mac] = c.get();
        std::printf("+ %s %s joined room \"%s\" (%zu in room, %zu total)\n", addr_str(from).c_str(),
                    mac_str(mac).c_str(), room.c_str(), members2.size(), g_clients.size() + 1);
        g_clients[key] = std::move(c);
    }
    Client* c = g_clients[key].get();
    uint8_t ok[3];
    uint16_t n = (uint16_t)g_rooms[c->room].size();
    std::memcpy(ok, &n, 2);
    ok[2] = VERSION;
    send_relay(from, OP_OK, ok, 3);
}

bool allow(Client& c, size_t bytes) {
    const int64_t t = now_us();
    const double dt = (t - c.refill_at) / 1e6;
    c.refill_at = t;
    c.pkt_tokens = std::min(cfg.pps * 2, c.pkt_tokens + dt * cfg.pps);
    c.byte_tokens = std::min(cfg.kbps * 125 * 2, c.byte_tokens + dt * cfg.kbps * 125);
    if (c.pkt_tokens < 1 || c.byte_tokens < (double)bytes) return false;
    c.pkt_tokens -= 1; c.byte_tokens -= (double)bytes;
    return true;
}

void handle_packet(const uint8_t* d, size_t n, const sockaddr_in& from) {
    if (n < HDR || std::memcmp(d, "DSAH", 4) != 0) return;
    const uint8_t type = d[4];
    Mac src; std::memcpy(src.data(), d + 5, 6);
    uint16_t blen; std::memcpy(&blen, d + 11, 2);
    if (HDR + blen > n) return;
    const uint8_t* body = d + HDR;

    if (type == T_RELAY) {
        if (blen < 1) return;
        if (body[0] == OP_HELLO) handle_hello(from, src, body, blen);
        else if (body[0] == OP_BYE) {
            auto it = g_clients.find(addr_key(from));
            if (it != g_clients.end() && it->second->mac == src) drop_client(addr_key(from), "bye");
        }
        return;
    }
    if (type < T_BEACON || type > T_PTP) { if (type != T_FRAG) return; }

    auto it = g_clients.find(addr_key(from));
    if (it == g_clients.end() || it->second->mac != src) return;   // unregistered or spoofed MAC
    Client& c = *it->second;
    c.last_seen = now_us();
    c.rx++; g_total_rx++;
    if (!allow(c, n)) { c.dropped++; g_total_drop++; return; }

    auto& members = g_rooms[c.room];
    auto forward = [&](Client* to) {
        send_to(to->addr, d, n);
        c.fwd++; g_total_fwd++;
    };
    if (type == T_BEACON) {
        for (auto& [m, o] : members) if (o != &c) forward(o);
        return;
    }
    if (blen < 6) return;
    Mac dst; std::memcpy(dst.data(), body, 6);
    bool bcast = true;
    for (auto b : dst) if (b != 0xff) bcast = false;
    if (bcast) {
        for (auto& [m, o] : members) if (o != &c) forward(o);
    } else {
        auto mit = members.find(dst);
        if (mit != members.end() && mit->second != &c) forward(mit->second);
    }
}

void housekeeping() {
    const int64_t t = now_us();
    std::vector<uint64_t> dead;
    for (auto& [k, c] : g_clients) if (t - c->last_seen > CLIENT_TIMEOUT_US) dead.push_back(k);
    for (uint64_t k : dead) drop_client(k, "timeout");
}

void on_signal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") cfg.port = (uint16_t)std::atoi(next());
        else if (a == "--bind") cfg.bind = next();
        else if (a == "--max-room") cfg.max_room = (size_t)std::atoi(next());
        else if (a == "--max-clients") cfg.max_clients = (size_t)std::atoi(next());
        else if (a == "--pps") cfg.pps = std::atof(next());
        else if (a == "--kbps") cfg.kbps = std::atof(next());
        else if (a == "--verbose") cfg.verbose = true;
        else { std::fprintf(stderr, "usage: %s [--port N] [--bind IP] [--max-room N] [--max-clients N] [--pps N] [--kbps N] [--verbose]\n", argv[0]); return 2; }
    }
    FILE* ur = std::fopen("/dev/urandom", "rb");
    if (!ur || std::fread(g_key, 1, sizeof g_key, ur) != sizeof g_key) { std::fprintf(stderr, "no entropy\n"); return 1; }
    std::fclose(ur);

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { std::perror("socket"); return 1; }
    int big = 4 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    timeval tv{1, 0};
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.bind.c_str(), &a.sin_addr) != 1) { std::fprintf(stderr, "bad --bind\n"); return 2; }
    if (bind(g_sock, (sockaddr*)&a, sizeof a) != 0) { std::perror("bind"); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("adhoc_relay listening on %s:%u (rooms of up to %zu, %zu clients, %.0f pkt/s and %.0f kbit/s per client)\n",
                cfg.bind.c_str(), cfg.port, cfg.max_room, cfg.max_clients, cfg.pps, cfg.kbps);

    static uint8_t buf[MAX_DATAGRAM + 1];
    int64_t next_house = now_us() + 1000000, next_stats = now_us() + 60000000;
    uint64_t last_rx = 0, last_fwd = 0;
    while (!g_stop) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(g_sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n > 0 && (size_t)n <= MAX_DATAGRAM) handle_packet(buf, (size_t)n, from);
        const int64_t t = now_us();
        if (t >= next_house) { next_house = t + 1000000; housekeeping(); }
        if (t >= next_stats) {
            next_stats = t + 60000000;
            std::printf("stats: %zu clients in %zu rooms; last minute rx %llu forwarded %llu; dropped (rate limit) total %llu\n",
                        g_clients.size(), g_rooms.size(), (unsigned long long)(g_total_rx - last_rx),
                        (unsigned long long)(g_total_fwd - last_fwd), (unsigned long long)g_total_drop);
            last_rx = g_total_rx; last_fwd = g_total_fwd;
        }
    }
    std::printf("shutting down\n");
    return 0;
}
