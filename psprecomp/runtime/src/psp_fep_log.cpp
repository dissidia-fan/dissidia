// psp_fep_log.cpp -- PSPRECOMP_FEP_LOG (psp_fep_log.h).
#include "psp_fep_log.h"
#ifndef PSP_FEP_LOG_PURE
#include "psp_ge.h"
#include "psp_ge_vertex.h"
#endif
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void psp_fep_log_measure(const float W[12], const float V[12], const float* pos, int n, FepLogMeasure* o) {
    std::memset(o, 0, sizeof *o);
    for (int c = 0; c < 3; c++) {
        o->org[c] = W[9 + c];
        o->scale[c] = std::sqrt(W[c * 3] * W[c * 3] + W[c * 3 + 1] * W[c * 3 + 1] + W[c * 3 + 2] * W[c * 3 + 2]);
        o->cam[c] = -(V[c * 3] * V[9] + V[c * 3 + 1] * V[10] + V[c * 3 + 2] * V[11]);   // -R^T t
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    float rmax = 0.f, dmin = 1e30f;
    for (int i = 0; i < n; i++) {
        const float* p = pos + 3 * i;
        float w[3];
        for (int r = 0; r < 3; r++) w[r] = W[r] * p[0] + W[3 + r] * p[1] + W[6 + r] * p[2] + W[9 + r];
        float r2 = 0.f, d2 = 0.f;
        for (int r = 0; r < 3; r++) {
            if (w[r] < lo[r]) lo[r] = w[r];
            if (w[r] > hi[r]) hi[r] = w[r];
            r2 += (w[r] - o->org[r]) * (w[r] - o->org[r]);
            d2 += (w[r] - o->cam[r]) * (w[r] - o->cam[r]);
        }
        rmax = std::fmax(rmax, std::sqrt(r2));
        dmin = std::fmin(dmin, std::sqrt(d2));
    }
    if (n <= 0) { for (int r = 0; r < 3; r++) lo[r] = hi[r] = o->org[r]; dmin = 0.f; }
    float d2 = 0.f;
    for (int r = 0; r < 3; r++) {
        o->ctr[r] = 0.5f * (lo[r] + hi[r]);
        o->half[r] = 0.5f * (hi[r] - lo[r]);
        d2 += (o->cam[r] - o->org[r]) * (o->cam[r] - o->org[r]);
    }
    o->rmax = rmax;
    o->d_org = std::sqrt(d2);
    o->d_min = dmin;
    o->inside = (n > 0 && o->d_org < rmax) ? 1 : 0;
}

int psp_fep_log_parse(const char* e, uint64_t* ids, int max_ids, int* n_ids) {
    *n_ids = 0;
    if (!e || !*e || std::strcmp(e, "0") == 0) return 0;
    if (std::strcmp(e, "1") == 0 || std::strcmp(e, "big") == 0) return 1;
    if (std::strcmp(e, "all") == 0) return 2;
    const char* s = e;
    while (*s && *n_ids < max_ids) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s, &end, 16);
        if (end == s) break;
        ids[(*n_ids)++] = (uint64_t)v;
        s = end;
        while (*s == ',' || *s == ' ') s++;
    }
    return *n_ids ? 3 : 1;
}

#ifndef PSP_FEP_LOG_PURE   // the runtime side (the host test builds only the pure functions above)
namespace {
int g_mode = -1, g_nids = 0;
uint64_t g_ids[64];
long g_left = 40000;
std::chrono::steady_clock::time_point g_t0;
void init() {
    g_mode = psp_fep_log_parse(std::getenv("PSPRECOMP_FEP_LOG"), g_ids, 64, &g_nids);
    if (const char* m = std::getenv("PSPRECOMP_FEP_LOG_MAX")) g_left = std::atol(m);
    g_t0 = std::chrono::steady_clock::now();
    if (g_mode > 0)
        std::fprintf(stderr, "[FEPLOG] on: mode %s, %d ids, max %ld lines. Fields: t ms, texpack id, prim, verts, "
                     "org = node position, scale = WORLD column lengths, half = world AABB half-size, r = largest "
                     "vertex distance from org, cam = eye, d_org / d_min = eye to org / nearest vertex, in = eye "
                     "inside r (all metres)\n",
                     g_mode == 1 ? "big (>= 1 m)" : g_mode == 2 ? "all" : "ids", g_nids, g_left);
}
}  // namespace

bool psp_fep_log_on() {
    if (g_mode < 0) init();
    return g_mode > 0 && g_left > 0;
}

void psp_fep_log_draw(uint8_t* rdram, const GeState& st, int prim_type, int count, uint64_t tex_id) {
    // PSPRECOMP_FEP_LOG_SKIN=1 also measures skinned draws (GMO effect parts: one bone per mesh), with their positions
    // first put through GE bone matrix 0 (the draw's first skinning matrix; exact for single-bone meshes).
    static const bool skin = std::getenv("PSPRECOMP_FEP_LOG_SKIN") && std::strcmp(std::getenv("PSPRECOMP_FEP_LOG_SKIN"), "0") != 0;
    const bool weighted = ge_vtype_weight(st.vertex_type) != 0;
    if (!psp_fep_log_on() || !st.texture_enable || ge_vtype_through(st.vertex_type) || (weighted && !skin))
        return;
    if (g_mode == 3) {
        bool hit = false;
        for (int i = 0; i < g_nids && !hit; i++) hit = g_ids[i] == tex_id;
        if (!hit) return;
    }
    static thread_local std::vector<DecodedVertex> dv;
    static thread_local std::vector<float> p;
    ge_decode_vertices(rdram, st, prim_type, count, dv);
    p.resize(dv.size() * 3);
    for (size_t i = 0; i < dv.size(); i++) {
        if (weighted) {   // bone matrix 0, 4x3 column-major like WORLD
            const float* B = st.bone_matrix; const float* q = dv[i].pos;
            for (int r = 0; r < 3; r++) p[3 * i + r] = B[r] * q[0] + B[3 + r] * q[1] + B[6 + r] * q[2] + B[9 + r];
        } else {
            std::memcpy(&p[3 * i], dv[i].pos, sizeof(float) * 3);
        }
    }
    FepLogMeasure m;
    psp_fep_log_measure(st.world_matrix, st.view_matrix, p.data(), (int)dv.size(), &m);
    if (g_mode == 1 && std::fmax(m.half[0], std::fmax(m.half[1], m.half[2])) < 1.0f) return;
    const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count();
    std::fprintf(stderr, "[FEPLOG] t %.0f id %016llx%s prim %d n %d org (%.2f,%.2f,%.2f) scale (%.2f,%.2f,%.2f) "
                 "half (%.2f,%.2f,%.2f) r %.2f cam (%.2f,%.2f,%.2f) d_org %.2f d_min %.2f in %d\n",
                 t, (unsigned long long)tex_id, weighted ? " skin" : "", prim_type, (int)dv.size(), m.org[0], m.org[1], m.org[2],
                 m.scale[0], m.scale[1], m.scale[2], m.half[0], m.half[1], m.half[2], m.rmax,
                 m.cam[0], m.cam[1], m.cam[2], m.d_org, m.d_min, m.inside);
    g_left--;
}
#endif
