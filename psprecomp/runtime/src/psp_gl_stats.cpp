// PSPRECOMP_GL_STATS=1: per-second GL call counters.
//
// glad exposes every GL entry point as a global function pointer
// (glad_glFoo). When the env var is set, install_gl_stats() replaces each
// loaded pointer with a counting trampoline that forwards to the driver, so
// every call site in the runtime is covered without touching the call sites.
// Cost when the variable is unset: none (nothing is installed).
//
// PSPRECOMP_GL_STATS=2 additionally polls glGetError after every call and logs
// the first 40 errors as "[GLERR] 0xNNNN after <function>" (strictness check; slow).
//
// Output, once per second on the GL thread (hooked into ge_present_frame):
//   [GLSTATS] 28.4 frames/s | calls/frame=412 draws=96 uniforms=130 bufupload=12 \
//             binds=88 state=40 sync=4 | top: glUniform1i=90 ...
#include <glad/glad.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxFns = 400;
struct Counter { const char* name; uint64_t n; };
Counter g_ctr[kMaxFns];
int g_nctr = 0;
bool g_on = false;
bool g_check = false;                       // PSPRECOMP_GL_STATS=2: glGetError after every call
PFNGLGETERRORPROC g_real_get_error = nullptr;
uint64_t g_err_total = 0;
void report_error(const char* fn) {
    GLenum e;
    while (g_real_get_error && (e = g_real_get_error()) != GL_NO_ERROR) {
        g_err_total++;
        if (g_err_total <= 40)
            std::fprintf(stderr, "[GLERR] 0x%04X after %s (error #%llu)\n", e, fn,
                         (unsigned long long)g_err_total);
    }
}

template <typename F> struct Sig;
template <typename R, typename... A> struct Sig<R (*)(A...)> {
    using Fn = R (*)(A...);
    template <int Id> struct Tramp {
        static inline Fn real = nullptr;
        static R call(A... a) {
            g_ctr[Id].n++;
            if constexpr (std::is_void_v<R>) {
                real(std::forward<A>(a)...);
                if (g_check) report_error(g_ctr[Id].name);
            } else {
                R r = real(std::forward<A>(a)...);
                if (g_check) report_error(g_ctr[Id].name);
                return r;
            }
        }
    };
};
// One distinct template instantiation per function, keyed by __COUNTER__.
#define X(fn)                                                                  \
    {                                                                          \
        using P = decltype(glad_##fn);                                         \
        if (glad_##fn) {                                                       \
            constexpr int Id = __COUNTER__;                                    \
            using T = typename Sig<P>::template Tramp<Id>;                     \
            T::real = glad_##fn;                                               \
            g_ctr[Id] = Counter{#fn, 0};                                       \
            if (Id + 1 > g_nctr) g_nctr = Id + 1;                              \
            glad_##fn = &T::call;                                              \
        }                                                                      \
    }

} // namespace

bool gl_stats_enabled() { return g_on; }

void gl_stats_install() {
    if (!std::getenv("PSPRECOMP_GL_STATS") || g_on) return;
    g_on = true;
    g_check = std::atoi(std::getenv("PSPRECOMP_GL_STATS")) >= 2;
    g_real_get_error = glad_glGetError;
    if (g_check) while (g_real_get_error() != GL_NO_ERROR) {}
#include "psp_gl_stats.inc"
    std::fprintf(stderr, "[GLSTATS] counting %d GL entry points%s\n", g_nctr, g_check ? " (+ glGetError after every call)" : "");
}

static bool starts(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

/// Called once per presented frame. Prints and resets once a second.
void gl_stats_frame() {
    if (!g_on) return;
    using clk = std::chrono::steady_clock;
    static auto t0 = clk::now();
    static uint32_t frames = 0;
    frames++;
    auto now = clk::now();
    double dt = std::chrono::duration<double>(now - t0).count();
    if (dt < 1.0) return;
    uint64_t total = 0, draws = 0, unif = 0, upload = 0, binds = 0, state = 0, sync = 0, tex = 0;
    std::vector<std::pair<uint64_t, const char*>> top;
    for (int i = 0; i < g_nctr; i++) {
        Counter& c = g_ctr[i];
        if (!c.name || !c.n) continue;
        total += c.n;
        top.emplace_back(c.n, c.name);
        if (starts(c.name, "glDraw") || starts(c.name, "glMultiDraw")) draws += c.n;
        else if (starts(c.name, "glUniform")) unif += c.n;
        else if (!std::strcmp(c.name, "glBufferData") || !std::strcmp(c.name, "glBufferSubData")
                 || !std::strcmp(c.name, "glMapBufferRange") || !std::strcmp(c.name, "glUnmapBuffer")
                 || !std::strcmp(c.name, "glFlushMappedBufferRange")) upload += c.n;
        else if (starts(c.name, "glTexImage") || starts(c.name, "glTexSubImage")) tex += c.n;
        else if (starts(c.name, "glBind") || starts(c.name, "glUseProgram") || starts(c.name, "glActiveTexture")) binds += c.n;
        else if (starts(c.name, "glFenceSync") || starts(c.name, "glClientWaitSync") || starts(c.name, "glDeleteSync")
                 || !std::strcmp(c.name, "glFinish") || !std::strcmp(c.name, "glFlush")
                 || !std::strcmp(c.name, "glGetError") || starts(c.name, "glGet") || starts(c.name, "glRead")) sync += c.n;
        else if (starts(c.name, "glEnable") || starts(c.name, "glDisable") || starts(c.name, "glBlend")
                 || starts(c.name, "glDepth") || starts(c.name, "glCull") || starts(c.name, "glColorMask")
                 || starts(c.name, "glViewport") || starts(c.name, "glScissor") || starts(c.name, "glStencil")) state += c.n;
        c.n = 0;
    }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    const double f = frames ? frames : 1;
    std::fprintf(stderr,
        "[GLSTATS] %.1f frames/s | per frame: calls=%.0f draws=%.0f uniform=%.0f upload=%.0f tex=%.0f bind=%.0f state=%.0f sync/query=%.0f | top:",
        frames / dt, total / f, draws / f, unif / f, upload / f, tex / f, binds / f, state / f, sync / f);
    for (size_t i = 0; i < top.size() && i < 6; i++)
        std::fprintf(stderr, " %s=%.0f", top[i].second, top[i].first / f);
    std::fprintf(stderr, "\n");
    frames = 0;
    t0 = now;
}
