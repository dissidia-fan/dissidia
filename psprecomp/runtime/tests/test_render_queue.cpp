// Unit tests for the render queue's GE list FIFO (psp_render_queue.cpp): the
// stall / resume bookkeeping between the game thread (enqueue, UpdateStallAddr)
// and the main thread (render_queue_process).
//
// Standalone executable: links psp_render_queue.cpp with the GE interpreter,
// the presenter and the scheduler stubbed below -- no SDL2/GL.

#include "psp_render_queue.h"
#include "psp_ge.h"
#include "psp_ge_draw.h"

#include <atomic>
#include <cstdio>
#include <functional>

std::atomic<bool> g_should_exit{false};
void sched_yield_point() {}
void ge_present_frame(uint8_t*, uint32_t, uint32_t, uint32_t) {}

// The GE interpreter: runs a list from `pc` to its stall address, or to its end
// at k_list_end. `during` runs while it does, as the game thread can: the main
// thread runs lists with the queue unlocked.
static constexpr uint32_t k_list_end = 0x300;
static std::function<void()> during;
static int runs = 0;

GeListResult ge_process_display_list(uint8_t*, uint32_t pc, uint32_t stall) {
    runs++;
    if (during) {
        const auto f = std::move(during);
        during = nullptr;
        f();
    }
    GeListResult r;
    r.completed = stall == 0 || stall >= k_list_end;
    r.stopped_pc = r.completed ? k_list_end : stall;
    return r;
}

static int failures = 0;
static int tests_run = 0;

#define CHECK(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

// A list that reaches its stall waits there until UpdateStallAddr moves it.
static void test_stall_then_resume() {
    const int uid = render_queue_enqueue_ge_list(nullptr, 0x100, 0x200);
    render_queue_process();
    CHECK(render_queue_has_pending_ge(), "stalled at 0x200: still pending");
    const int before = runs;
    render_queue_process();
    CHECK(runs == before, "a stalled list is not run again before its stall moves");
    render_queue_update_stall(uid, 0x300);
    render_queue_process();
    CHECK(render_queue_all_ge_done(), "stall moved to the end: the list is done");
}

// The game moves the stall while the main thread runs the list: the run stops at
// the old stall, and the list must still go on to the new one. (Writing the run's
// "stalled" back over the unstall left it waiting for an update that never came:
// every DrawSync timed out, 2 frames/s from boot.)
static void test_stall_moved_while_running() {
    const int uid = render_queue_enqueue_ge_list(nullptr, 0x100, 0x200);
    during = [uid] { render_queue_update_stall(uid, 0x300); };
    render_queue_process();
    render_queue_process();
    CHECK(render_queue_all_ge_done(), "stall moved mid-run: the list runs on to the new stall");
}

// A list without a stall address runs to its end in one go.
static void test_no_stall() {
    render_queue_enqueue_ge_list(nullptr, 0x100, 0);
    render_queue_process();
    CHECK(render_queue_all_ge_done(), "no stall: done after one run");
    CHECK(render_queue_draw_sync(1) == 0, "DrawSync poll: nothing pending");
}

int main() {
    test_stall_then_resume();
    test_stall_moved_while_running();
    test_no_stall();

    if (failures == 0) {
        std::printf("test_render_queue: %d/%d PASS\n", tests_run, tests_run);
        return 0;
    }
    std::fprintf(stderr, "test_render_queue: %d/%d FAIL\n",
        tests_run - failures, tests_run);
    return 1;
}
