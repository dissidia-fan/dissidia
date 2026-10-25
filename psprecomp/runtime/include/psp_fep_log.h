// psp_fep_log.h -- ground-truth log of effect draws (PSPRECOMP_FEP_LOG).
//
// Logs, per GPU-transformed draw, the world-space size and position the GE actually draws (the decoded
// model-space vertices through the draw's own WORLD matrix), the camera position (from the VIEW matrix) and
// the camera's distance to it. FEP effect nodes reach the GE as one draw per node with the node's scale,
// rotation and position baked into WORLD, so this is what the engine evaluated, not what our tools expect.
// Off by default: one cached bool per draw when off.
//
//   PSPRECOMP_FEP_LOG=1       textured, unskinned draws whose world half-size is >= 1 m (effect meshes, cards)
//   PSPRECOMP_FEP_LOG=all     every textured, unskinned 3D draw
//   PSPRECOMP_FEP_LOG=<hex>[,<hex>...]   only these texture-pack ids (16 hex digits, as in fep_layers.json)
//   PSPRECOMP_FEP_LOG_MAX=<n> stop after n lines (default 40000)
// Lines go to stderr (tools/run.sh copies them to work/last_run.log), prefixed [FEPLOG].
#pragma once
#include <cstdint>

struct GeState;

struct FepLogMeasure {
    float org[3];        // the WORLD matrix's origin (the node's position)
    float scale[3];      // WORLD column lengths (the node's scale x the mesh's dequantisation)
    float ctr[3];        // world-space AABB centre of the drawn vertices
    float half[3];       // world-space AABB half-size
    float rmax;          // largest distance of a drawn vertex from org
    float cam[3];        // eye position from the VIEW matrix
    float d_org;         // eye -> org
    float d_min;         // eye -> nearest drawn vertex
    int inside;          // 1 when the eye is within rmax of org (the camera sits inside the shape's sphere)
};

/// Pure: measure model-space positions `pos` (n x 3) drawn with `world` / `view` (4x3 column-major, GE layout).
void psp_fep_log_measure(const float world[12], const float view[12], const float* pos, int n, FepLogMeasure* out);

/// Config parse (pure): returns mode 0 off, 1 big (>= 1 m), 2 all, 3 id list (ids/n_ids filled, at most max_ids).
int psp_fep_log_parse(const char* env, uint64_t* ids, int max_ids, int* n_ids);

/// True when PSPRECOMP_FEP_LOG is set (cached).
bool psp_fep_log_on();

/// The draw hook (psp_ge_draw.cpp, GPU path, after the batch's texture is bound).
void psp_fep_log_draw(uint8_t* rdram, const GeState& state, int prim_type, int count, uint64_t tex_id);
