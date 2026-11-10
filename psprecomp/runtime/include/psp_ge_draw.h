#pragma once
#include <cstdint>

/// Computed GL viewport rectangle (FBO pixels, GL bottom-left origin) and
/// depth range, derived from the PSP viewport scale/offset + depth registers.
struct GeViewportDepth {
    int x, y, w, h;       // glViewport args (FBO pixels)
    float near_z, far_z;  // glDepthRange args, clamped to [0,1]
};

/// Pure math: PSP viewport scale/offset + depth -> GL viewport + depth range
/// (issue #23). Mirrors PPSSPP ConvertViewportAndScissor for the 1:1,
/// non-supersampled, non-accurate-depth FBO case. Separated from the GL call
/// site (apply in ge_draw_prim) so it is unit-testable without a GL context.
/// off_x_raw/off_y_raw are the raw OFFSETX/OFFSETY register words (1/16
/// subpixel, low 16 bits used). fb_height is the render-target height in
/// pixels (PSP top-left origin is flipped to GL bottom-left).
GeViewportDepth ge_compute_viewport_depth(
    float vp_x_scale, float vp_y_scale,
    float vp_x_center, float vp_y_center,
    float vp_z_scale, float vp_z_center,
    uint32_t off_x_raw, uint32_t off_y_raw,
    int fb_height);

/// Internal render scale n (PSPRECOMP_RENDER_SCALE, 1..8): every target is
/// 480n x 272n; viewports are multiplied by n. Valid after ge_draw_init().
int ge_draw_render_scale();

/// Change the render scale live (settings menu): resizes every render target,
/// keeping its picture. Main (GL) thread, game paused. False if refused.
bool ge_draw_set_render_scale(int n);

/// Settings menu, Debug page (main/GL thread, game paused; not saved).
/// GPU sync mode, as PSPRECOMP_GE_SYNC: 0 off, 1 glFinish per batch, 2 per
/// display list, 3 per present, 4 glFlush per batch.
int ge_draw_sync_mode();
void ge_draw_set_sync_mode(int mode);
/// Vertex upload mode in use: 1 map, 0 sub (PSPRECOMP_GE_RING; changes at launch).
int ge_draw_ring_mode();
/// FX shader overrides: available = a manifest with usable entries was loaded
/// at start (PSPRECOMP_FX_SHADERS=0 or no manifest -> false). The live switch
/// only bypasses them; turning them back on needs no reload.
bool ge_draw_fx_available();
bool ge_draw_fx_shaders();
void ge_draw_set_fx_shaders(bool on);
/// Performance overlay: presented frames per second, average and worst frame
/// time, in the top-left corner (PSPRECOMP_PERF_HUD=1 turns it on at start).
bool ge_draw_perf_overlay();
void ge_draw_set_perf_overlay(bool on);

/// A short message over the picture, top centre, for `ms` milliseconds (a new call replaces the old
/// message). Any thread. Used for "frame rate set to 30 fps" when a multiplayer group agrees on one.
void ge_draw_toast(const char* text, int ms);

/// Present the last frame with the dialog/menu overlay now (game paused).
/// Main (GL) thread.
void ge_draw_present_overlay_now();

/// Initialize the GE draw infrastructure: FBO, VAO/VBO, shader.
/// Must be called after SDL/GL init and after ge_init()/ge_texture_init().
void ge_draw_init();

/// Shutdown the GE draw infrastructure.
void ge_draw_shutdown();

/// Called at start of display list processing (bind FBO, etc.)
void ge_draw_begin_list();

/// Called at end of display list processing.
void ge_draw_end_list();

/// Bind the GL FBO for the guest framebuffer address fb_addr_raw (FRAMEBUFPTR
/// or sceDisplaySetFrameBuf), creating it in the generic per-address pool if
/// needed (issue #59). key==0 keeps the current target. Render (GL) thread.
void ge_draw_select_target(uint32_t fb_addr_raw);
void ge_draw_note_framebufptr();   // a GE_CMD_FRAMEBUFPTR was decoded (key 0 is now real)

/// Colour texture of the pool target whose normalised VRAM key matches (0 if
/// none, or if it is the current render target). w/h receive the target's
/// pixel size, 480*n x 272*n for render scale n.
unsigned int ge_draw_find_target_texture(uint32_t key, int* w, int* h);

/// Mark the front buffer dirty (a list was processed) for the liveness net.
void ge_draw_mark_dirty();

/// Liveness net: if content is dirty and no real page-flip happened within
/// PSPRECOMP_PRESENT_STALE_MS (default 100ms), present the front buffer once.
/// MUST be called from the main (GL) thread — wired into psp_event_loop.
void ge_draw_present_if_stale(uint8_t* rdram);

/// Full draw pipeline for a GE PRIM command:
/// decode vertices -> transform -> upload VBO -> set GL state -> draw.
void ge_draw_prim(
    uint8_t* rdram,
    int prim_type,
    int count
);

/// Present the FBO to the SDL2 window via glBlitFramebuffer.
/// Does NOT read rdram for pixel data (FBO blit only).
void ge_present_frame(
    uint8_t* rdram,
    uint32_t fb_addr,
    uint32_t fb_stride,
    uint32_t fb_format
);

/// Snapshot the GE draw counters (issue #35 debug socket I command).
/// Lock-free racy reads of the draw-thread counters -- values may be a
/// few prims stale, which is fine for diagnostics. Any pointer may be null.
void ge_draw_get_stats(
    uint32_t* frames,
    int* prims_total,
    int* real_nonsprite,
    int* sprite_nonclear,
    int* clears
);

/// On-demand screenshot (issue #35 debug socket S command).
/// Callable from ANY thread: posts a capture request and blocks until the
/// render thread services it (or timeout_ms elapses). Returns true once
/// the TGA file has been written. Never touches GL itself.
bool ge_draw_capture_screenshot(const char* path, int timeout_ms);

/// Debug socket V command: capture the whole window (drawable size, with
/// letterbox bars) as a TGA. Same blocking contract as the screenshot call.
bool ge_draw_capture_window(const char* path, int timeout_ms);

/// Render-thread side of ge_draw_capture_screenshot: if a capture request
/// is pending, read the FBO and write the TGA. MUST be called from the
/// main (GL) thread -- wired into psp_event_loop next to
/// render_queue_process().
void ge_draw_service_screenshot_request();

/// Submit the pending batched draw, if any. Must be called (render thread)
/// before anything that depends on the draws having reached GL: display-list
/// exit / stall, render-target switches, clears, present, block transfers and
/// guest callbacks that may rewrite texture memory.
void ge_draw_flush();
void ge_draw_list_end();   // flush + optional per-list GPU sync (PSPRECOMP_GE_SYNC=2)
