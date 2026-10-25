// Host test for psp_fep_log (PSPRECOMP_FEP_LOG measuring): no GL, no guest memory (built with PSP_FEP_LOG_PURE).
#include "psp_fep_log.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

int main() {
    // a unit cube (+-1) scaled 2x and rotated 90 deg about y, at (1, 2, 3); eye at (0, 0, 10) looking down -z
    const float W[12] = {0, 0, -2,  0, 2, 0,  2, 0, 0,  1, 2, 3};      // columns: x' = -2z, y' = 2y, z' = 2x
    const float V[12] = {1, 0, 0,  0, 1, 0,  0, 0, 1,  0, 0, -10};     // R = I, t = -eye
    float pos[8 * 3];
    for (int i = 0; i < 8; i++) { pos[3 * i] = (i & 1) ? 1.f : -1.f; pos[3 * i + 1] = (i & 2) ? 1.f : -1.f; pos[3 * i + 2] = (i & 4) ? 1.f : -1.f; }
    FepLogMeasure m;
    psp_fep_log_measure(W, V, pos, 8, &m);
    CHECK(near(m.org[0], 1) && near(m.org[1], 2) && near(m.org[2], 3));
    CHECK(near(m.scale[0], 2) && near(m.scale[1], 2) && near(m.scale[2], 2));
    CHECK(near(m.half[0], 2) && near(m.half[1], 2) && near(m.half[2], 2));
    CHECK(near(m.ctr[0], 1) && near(m.ctr[1], 2) && near(m.ctr[2], 3));
    CHECK(near(m.rmax, 2 * std::sqrt(3.f)));
    CHECK(near(m.cam[0], 0) && near(m.cam[1], 0) && near(m.cam[2], 10));
    CHECK(near(m.d_org, std::sqrt(1.f + 4.f + 49.f)));
    CHECK(near(m.d_min, std::sqrt(1.f + 0.f + 25.f)));          // nearest drawn corner: (-1, 0, 5)
    CHECK(m.inside == 0);
    // the eye inside the shape
    const float V2[12] = {1, 0, 0,  0, 1, 0,  0, 0, 1,  -1, -2, -3.5f};   // eye (1, 2, 3.5)
    psp_fep_log_measure(W, V2, pos, 8, &m);
    CHECK(near(m.cam[2], 3.5f) && m.inside == 1);
    // a rotated view: eye position = -R^T t
    const float V3[12] = {0, 0, 1,  0, 1, 0,  -1, 0, 0,  0, 0, -5};       // R = rot y 90; t = (0, 0, -5)
    psp_fep_log_measure(W, V3, pos, 8, &m);
    CHECK(near(m.cam[0], 5) && near(m.cam[1], 0) && near(m.cam[2], 0));
    // config
    uint64_t ids[4]; int n = 0;
    CHECK(psp_fep_log_parse(nullptr, ids, 4, &n) == 0);
    CHECK(psp_fep_log_parse("0", ids, 4, &n) == 0);
    CHECK(psp_fep_log_parse("1", ids, 4, &n) == 1);
    CHECK(psp_fep_log_parse("all", ids, 4, &n) == 2);
    CHECK(psp_fep_log_parse("aa47040e80bcd4b3,422c4eed77b2a685", ids, 4, &n) == 3 && n == 2 &&
          ids[0] == 0xaa47040e80bcd4b3ull && ids[1] == 0x422c4eed77b2a685ull);
    std::printf("test_fep_log: %d/%d passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
