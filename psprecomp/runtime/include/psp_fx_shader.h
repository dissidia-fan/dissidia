// psp_fx_shader.h -- per-draw custom GLSL overrides for effects ("FX shaders").
//
// A draw whose bound texture has a texture-pack id (psp_tex_pack.h: the
// 64-bit hash of the decoded source texels + used palette, 16 hex digits as
// in textures.txt and the PSPRECOMP_TEX_DUMP filenames) listed in
//   <mods>/fx_shaders/manifest.json
// is drawn with that entry's fragment shader instead of the GE uber-shader.
// Geometry, the GE vertex transform, blend / depth / stencil / cull / colour
// mask stay the game's unless the entry overrides blend or depth.
// See FX_SHADERS.md for the manifest format and the standard uniforms.
//
// Split in two:
//   * psp_fx_manifest.cpp -- manifest parsing, matching, effect-age tracking
//     and the program registry (pure C++, host-tested: tests/test_fx_shader.cpp);
//   * psp_fx_shader.cpp   -- the GL side (compile, textures, uniforms).
//
// Env: PSPRECOMP_FX_SHADERS=0 disables the system; PSPRECOMP_FX_SHADER_LOG=1
// logs loads / compiles / matches (2 = also per-second GPU timing of the
// override draws); PSPRECOMP_FX_SHADERS_DIR overrides the folder;
// PSPRECOMP_FX_SHADER_RELOAD=1 re-reads the folder when manifest.json changes.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace psp_fx {

enum class Blend { Game, Add, AddOne, Alpha, Premul, Screen, Off };
enum class Depth { Game, NoWrite, Write, Off };

struct UniformValue {
    std::string name;
    int n = 0;              // 1..4 floats (float/vec2/vec3/vec4) or 16 (mat4)
    float v[16] = {};
};

struct TexBinding {
    std::string uniform;    // sampler2D name in the shader
    std::string path;       // PNG, relative to the manifest folder
    bool clamp = false;     // default repeat (the FFXV masks tile)
};

struct DrawEntry {
    std::string name;
    std::vector<uint64_t> hashes;
    std::string frag;                    // fragment shader file (relative)
    std::vector<std::string> includes;   // prepended GLSL files (relative)
    bool screen = false;                 // wants u_fx_screen (framebuffer copy)
    Blend blend = Blend::Game;
    Depth depth = Depth::Game;
    float life_ticks = 0.0f;             // u_fx_life_ticks; u_fx_progress = age / life
    float gap_ticks = 8.0f;              // unseen this long -> next sight is a new instance (age 0)
    float age_offset_ticks = 0.0f;       // added to the age (lab tick of the first sight)
    std::vector<UniformValue> uniforms;
    std::vector<TexBinding> textures;
    bool enabled = true;
};

struct Manifest {
    int version = 1;
    std::vector<DrawEntry> draws;
    std::vector<std::string> warnings;   // non-fatal problems (entry skipped, bad value)
};

/// Parse manifest JSON. False (and *err) only when the document itself is
/// unusable; bad entries are skipped with a warning.
bool parse_manifest(const std::string& json, Manifest& out, std::string* err);
/// "0x0123..." / "0123..." (1..16 hex digits). False on anything else.
bool parse_hash(const std::string& s, uint64_t* out);
std::string hash_str(uint64_t h);
bool parse_blend(const std::string& s, Blend* out);
bool parse_depth(const std::string& s, Depth* out);

/// Per-hash effect age. The first draw of a hash starts an instance; a draw
/// after the hash went unseen for more than gap_ticks starts a new one.
class AgeTracker {
public:
    /// Age in ticks (60/s) of the current instance of `h` at `now_s` seconds.
    /// *fresh is set when this sight started a new instance.
    float touch(uint64_t h, double now_s, float gap_ticks, bool* fresh = nullptr);
    void clear() { m_.clear(); }
    size_t size() const { return m_.size(); }
private:
    struct S { double first, last; };
    std::unordered_map<uint64_t, S> m_;
};

/// Program build inputs handed to the compiler callback.
struct ProgramSource {
    std::string frag;       // complete fragment source (preamble + includes + file)
    bool screen = false;
};

/// The compiled-program registry: entries whose program failed to build are
/// dropped (their draws keep the uber-shader).
class Registry {
public:
    /// Returns a program handle (> 0) or 0 with *log set.
    using CompileFn = std::function<unsigned(const ProgramSource&, std::string* log)>;
    /// Reads a file relative to the manifest folder; false if missing.
    using ReadFn = std::function<bool(const std::string& rel, std::string* data)>;
    using LogFn = std::function<void(const std::string&)>;

    struct Bound {
        const DrawEntry* entry = nullptr;
        unsigned program = 0;
        int index = 0;
    };

    /// Build every enabled entry. Returns the number of usable entries.
    int build(const Manifest& m, const ReadFn& read, const CompileFn& compile, const LogFn& log);
    const Bound* match(uint64_t hash) const {
        if (by_hash_.empty()) return nullptr;
        auto it = by_hash_.find(hash);
        return it == by_hash_.end() ? nullptr : &bound_[it->second];
    }
    bool empty() const { return bound_.empty(); }
    size_t size() const { return bound_.size(); }
    const Manifest& manifest() const { return m_; }
    /// Distinct programs (handle -> source key) for cleanup.
    const std::vector<unsigned>& programs() const { return programs_; }
    void clear() { m_ = Manifest{}; bound_.clear(); by_hash_.clear(); programs_.clear(); }

private:
    Manifest m_;
    std::vector<Bound> bound_;
    std::unordered_map<uint64_t, size_t> by_hash_;
    std::vector<unsigned> programs_;
};

/// The GLSL text the runtime puts in front of every override fragment shader
/// (version, inputs, standard uniforms, helpers). Exposed for tests/docs.
const char* fragment_preamble();
/// Wraps the user's fx_shade() into main() (alpha doubling for GE 2*As blends).
const char* fragment_epilogue();

}  // namespace psp_fx

// ---- GL side (psp_fx_shader.cpp; render thread only) ----
struct GeState;
struct GeXform3D;

/// Game-module seam: the fx_shaders folder (default <ms0 parent>/mods/fx_shaders).
void psp_fx_set_dir_provider(std::string (*fn)());

/// True when the system is enabled and a manifest exists (decided once, no
/// GL). Texture binds compute pack ids only when this (or the texture pack) is on.
bool psp_fx_want_ids();

struct PspFxDrawIn {
    uint64_t tex_id;          // texture-pack id of the bound texture (0 = none)
    const GeState* state;
    const GeXform3D* xform;   // GPU-transform draws; null = passthrough (CPU-transformed)
    int target_w, target_h;   // render target size in pixels
    int vp[4];                // GL viewport of the draw
    bool alpha_double;        // GE 2*As blend factors are emulated by doubling alpha
    unsigned (*screen_copy)(int* w, int* h);   // copy of the current target (once per frame)
};

struct PspFxDrawOut {
    psp_fx::Blend blend;
    psp_fx::Depth depth;
    bool timed;               // caller wraps the draw in psp_fx_timer_begin/end
};

/// Called once per batch after the uber pipeline is applied. On a match binds
/// the override program, its uniforms and textures (active texture unit is
/// left at 0) and returns true; the caller applies out->blend / out->depth.
bool psp_fx_begin_batch(const PspFxDrawIn& in, PspFxDrawOut* out);
void psp_fx_timer_begin();
void psp_fx_timer_end();
/// Once per presented frame (stats, screen-copy serial, hot reload).
void psp_fx_end_frame();
void psp_fx_shutdown();
