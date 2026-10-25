#pragma once
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include <cstdint>
#include <vector>

/// Decoded vertex in a uniform format for GL upload.
/// All PSP vertex formats are converted to this before rendering.
struct DecodedVertex {
    float pos[3];       // Position (x, y, z)
    float w = 1.0f;     // Clip-space w (transform mode); 1 otherwise
    float uv[2];        // Texture coordinates (u, v)
    uint8_t color[4];   // RGBA color
    float normal[3];    // Normal vector
    bool has_uv;
    bool has_color;
    bool has_normal;
};

/// Compute the byte stride of one PSP vertex based on VTYPE bitfield.
int ge_vertex_stride(uint32_t vtype);

/// Decode `count` vertices from rdram at state.vertex_addr using
/// the vertex format described by state.vertex_type. If index bits
/// are set in VTYPE, reads the index buffer from state.index_addr.
void ge_decode_vertices(
    uint8_t* rdram,
    const GeState& state,
    int prim_type,
    int count,
    std::vector<DecodedVertex>& out
);

/// Apply world*view*proj matrix to positions and tex scale/offset
/// to UVs. Through-mode vertices are mapped to NDC directly.
void ge_transform_vertices(
    std::vector<DecodedVertex>& verts,
    const GeState& state
);

// ---- Game-module degenerate-matrix fallback (issue #47 Phase 5 seam) ----
// When the guest uploads broken matrices (view all-zero; proj NaN/Inf or
// collapsed diagonal — open issue, FPU/VFPU dataflow family), the real
// transform path cannot work. The mapping that produces a usable frame
// anyway is GAME-TUNED (it depends on the title's intended projection),
// so it installs from the game module. Generic default: world-space
// passthrough (positions used as NDC unchanged) plus the one-time warns.
// Signature: world-space position in, NDC out.
using GeDegenerateFallbackFn = void (*)(const float wpos[3], float out[3]);
void ge_vertex_set_degenerate_fallback(GeDegenerateFallbackFn fn);

// ---- GPU transform path (vertex-shader skinning / lighting / MVP) ----

/// One model-space vertex for the GPU transform path. Skinning weights are
/// only meaningful when the vertex type has weights (first `wcount` entries).
struct GeGpuVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t color[4];
    float weights[8];
};
static_assert(sizeof(GeGpuVertex) == 68, "GeGpuVertex layout is shared with the VAO setup");

/// Decode `count` NON-through vertices (honouring the index buffer) into
/// model-space GeGpuVertex records with no CPU skinning / lighting /
/// transform. Weights are written only for weighted vertex types. Returns false
/// (nothing written) if guest memory for the vertices/indices is out of range;
/// the caller then takes the CPU path, which reproduces the NULL-page
/// semantics.
bool ge_decode_vertices_gpu(
    uint8_t* rdram, const GeState& state, int count, GeGpuVertex* out);

/// True when the real MVP path can run on the GPU for this draw: matrices are
/// sane and no game-module degenerate-matrix fallback is installed (such a
/// fallback needs the CPU world-space position).
bool ge_gpu_transform_ok(const GeState& state);
