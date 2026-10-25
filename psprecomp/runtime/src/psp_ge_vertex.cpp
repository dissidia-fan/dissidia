#include "psp_ge_vertex.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "recomp.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---- Matrix helpers (PSP column-major layout) ----
//
// The GE uploads matrices COLUMN-major: world/view are
// 4 columns x 3 rows (translation at flat indices 9,10,11);
// proj is 4 columns x 4 rows. These match PPSSPP's
// Vec3ByMatrix43 / Vec3ByMatrix44.

/// Transform (x,y,z,1) by a 4x3 column-major matrix.
static void vec3_by_matrix43(
    const float m[12], const float v[3], float out[3]
) {
    out[0] = v[0] * m[0] + v[1] * m[3]
           + v[2] * m[6] + m[9];
    out[1] = v[0] * m[1] + v[1] * m[4]
           + v[2] * m[7] + m[10];
    out[2] = v[0] * m[2] + v[1] * m[5]
           + v[2] * m[8] + m[11];
}

/// Transform (x,y,z,1) by a 4x4 column-major matrix.
static void vec3_by_matrix44(
    const float m[16], const float v[3], float out[4]
) {
    out[0] = v[0] * m[0] + v[1] * m[4]
           + v[2] * m[8] + m[12];
    out[1] = v[0] * m[1] + v[1] * m[5]
           + v[2] * m[9] + m[13];
    out[2] = v[0] * m[2] + v[1] * m[6]
           + v[2] * m[10] + m[14];
    out[3] = v[0] * m[3] + v[1] * m[7]
           + v[2] * m[11] + m[15];
}

// ---- Vertex stride calculation ----

namespace {
/// Byte offsets of each vertex component (PSP order: weights, texcoord,
/// colour, normal, position), each aligned to its own element size; the
/// total is aligned to the largest component alignment (PPSSPP
/// VertexDecoder::SetVertexType).
struct VtxLayout {
    int weight_off = 0, tc_off = 0, col_off = 0, nrm_off = 0, pos_off = 0;
    int stride = 0;
    int wt = 0, wcount = 0;
};

VtxLayout vtx_layout(uint32_t vtype) {
    VtxLayout L;
    int off = 0, maxalign = 1;
    auto place = [&](int size, int align) {
        off = (off + align - 1) & ~(align - 1);
        int at = off;
        off += size;
        if (align > maxalign) maxalign = align;
        return at;
    };
    L.wt = ge_vtype_weight(vtype);
    L.wcount = ((vtype >> GE_VTYPE_WEIGHTCOUNT_SHIFT)
                & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
    switch (L.wt) {
    case 1: L.weight_off = place(L.wcount, 1); break;
    case 2: L.weight_off = place(L.wcount * 2, 2); break;
    case 3: L.weight_off = place(L.wcount * 4, 4); break;
    default: break;
    }
    switch (ge_vtype_tc(vtype)) {
    case GE_VTYPE_TC_8BIT:  L.tc_off = place(2, 1); break;
    case GE_VTYPE_TC_16BIT: L.tc_off = place(4, 2); break;
    case GE_VTYPE_TC_FLOAT: L.tc_off = place(8, 4); break;
    default: break;
    }
    switch (ge_vtype_col(vtype)) {
    case GE_VTYPE_COL_565:
    case GE_VTYPE_COL_5551:
    case GE_VTYPE_COL_4444: L.col_off = place(2, 2); break;
    case GE_VTYPE_COL_8888: L.col_off = place(4, 4); break;
    default: break;
    }
    switch (ge_vtype_nrm(vtype)) {
    case GE_VTYPE_NRM_8BIT:  L.nrm_off = place(3, 1); break;
    case GE_VTYPE_NRM_16BIT: L.nrm_off = place(6, 2); break;
    case GE_VTYPE_NRM_FLOAT: L.nrm_off = place(12, 4); break;
    default: break;
    }
    switch (ge_vtype_pos(vtype)) {
    case GE_VTYPE_POS_8BIT:  L.pos_off = place(3, 1); break;
    case GE_VTYPE_POS_16BIT: L.pos_off = place(6, 2); break;
    case GE_VTYPE_POS_FLOAT: L.pos_off = place(12, 4); break;
    default: break;
    }
    L.stride = (off + maxalign - 1) & ~(maxalign - 1);
    return L;
}
}  // namespace

int ge_vertex_stride(uint32_t vtype) {
    return vtx_layout(vtype).stride;
}

// ---- Inline read helpers ----

static inline uint8_t read_u8(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint8_t>(rdram, addr);
}

static inline int8_t read_s8(
    uint8_t* rdram, uint32_t addr
) {
    return static_cast<int8_t>(
        psp_mem_read<uint8_t>(rdram, addr));
}

static inline uint16_t read_u16(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint16_t>(rdram, addr);
}

static inline int16_t read_s16(
    uint8_t* rdram, uint32_t addr
) {
    return static_cast<int16_t>(
        psp_mem_read<uint16_t>(rdram, addr));
}

static inline uint32_t read_u32(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint32_t>(rdram, addr);
}

static inline float read_float(
    uint8_t* rdram, uint32_t addr
) {
    uint32_t bits = psp_mem_read<uint32_t>(rdram, addr);
    float f;
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

// ---- Vertex decode ----

void ge_decode_vertices(
    uint8_t* rdram,
    const GeState& state,
    int prim_type,
    int count,
    std::vector<DecodedVertex>& out
) {
    uint32_t vtype = state.vertex_type;
    const VtxLayout L = vtx_layout(vtype);
    int stride = L.stride;

    if (stride <= 0 || count <= 0) return;

    int tc_type  = ge_vtype_tc(vtype);
    int col_type = ge_vtype_col(vtype);
    int nrm_type = ge_vtype_nrm(vtype);
    int pos_type = ge_vtype_pos(vtype);
    int idx_type = ge_vtype_idx(vtype);

    // Read index buffer if indexed
    static thread_local std::vector<uint32_t> indices;
    indices.clear();
    if (idx_type != GE_VTYPE_IDX_NONE) {
        indices.resize(count);
        uint32_t iaddr = state.index_addr;
        for (int i = 0; i < count; i++) {
            switch (idx_type) {
            case GE_VTYPE_IDX_8BIT:
                indices[i] = read_u8(rdram, iaddr);
                iaddr += 1;
                break;
            case GE_VTYPE_IDX_16BIT:
                indices[i] = read_u16(rdram, iaddr);
                iaddr += 2;
                break;
            case GE_VTYPE_IDX_32BIT:
                indices[i] = read_u32(rdram, iaddr);
                iaddr += 4;
                break;
            default:
                indices[i] = i;
                break;
            }
        }
    }

    out.resize(count);
    for (int i = 0; i < count; i++) {
        DecodedVertex& v = out[i];
        // Every field of the vertex is (re)written here, w included: callers
        // reuse `out` across PRIMs (the fast path's thread_local `decoded`), and
        // resize() keeps the old elements. w is only ever set again by the
        // real-MVP branch of ge_transform_vertices, so without this reset a
        // through-mode / NDC-direct vertex inherited the clip w of whatever
        // vertex last used its slot -- e.g. the w=0 "behind the eye" marker
        // rect_clip_to_ndc leaves on a 3D sprite -- and the GPU divided the
        // HUD by it (Dissidia: whole HUD smeared radially to the screen edges
        // while dashing at the EX Core; see DEBUGGING.md "HUD smeared ...").
        v.w = 1.0f;
        v.has_uv = (tc_type != GE_VTYPE_TC_NONE);
        v.has_color = (col_type != GE_VTYPE_COL_NONE);
        v.has_normal = (nrm_type != GE_VTYPE_NRM_NONE);

        // Determine vertex index
        uint32_t vidx = (idx_type != GE_VTYPE_IDX_NONE)
                         ? indices[i]
                         : static_cast<uint32_t>(i);

        uint32_t base = state.vertex_addr
                        + vidx * static_cast<uint32_t>(stride);
        uint32_t offset = 0;

        // Skinning weights (first in the vertex)
        float weights[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const int wn = std::min(L.wcount, 8);
        if (L.wt != 0) {
            for (int k = 0; k < wn; k++) {
                if (L.wt == 1)
                    weights[k] = read_u8(rdram, base + L.weight_off + k) / 128.0f;
                else if (L.wt == 2)
                    weights[k] = read_u16(rdram, base + L.weight_off + k * 2) / 32768.0f;
                else
                    weights[k] = read_float(rdram, base + L.weight_off + k * 4);
            }
        }

        // Texture coordinates
        offset = L.tc_off;
        // In through mode 8/16-bit texcoords are raw texel units
        // (PPSSPP Step_TcU8Through / Step_TcU16Through); otherwise
        // they are fixed point (/128, /32768).
        const bool through = (state.vertex_type & 0x800000u) != 0;
        if (tc_type == GE_VTYPE_TC_8BIT) {
            float a = static_cast<float>(read_u8(rdram, base + offset));
            float b = static_cast<float>(read_u8(rdram, base + offset + 1));
            v.uv[0] = through ? a : a / 128.0f;
            v.uv[1] = through ? b : b / 128.0f;
            offset += 2;
        } else if (tc_type == GE_VTYPE_TC_16BIT) {
            float a = static_cast<float>(read_u16(rdram, base + offset));
            float b = static_cast<float>(read_u16(rdram, base + offset + 2));
            v.uv[0] = through ? a : a / 32768.0f;
            v.uv[1] = through ? b : b / 32768.0f;
            offset += 4;
        } else if (tc_type == GE_VTYPE_TC_FLOAT) {
            v.uv[0] = read_float(rdram, base + offset);
            v.uv[1] = read_float(rdram, base + offset + 4);
            offset += 8;
        } else {
            v.uv[0] = 0.0f;
            v.uv[1] = 0.0f;
        }

        // Color
        offset = L.col_off;
        if (col_type == GE_VTYPE_COL_565) {
            uint16_t val = read_u16(rdram, base + offset);
            v.color[0] = static_cast<uint8_t>(
                ((val & 0x1F) * 255) / 31);
            v.color[1] = static_cast<uint8_t>(
                (((val >> 5) & 0x3F) * 255) / 63);
            v.color[2] = static_cast<uint8_t>(
                (((val >> 11) & 0x1F) * 255) / 31);
            v.color[3] = 255;
            offset += 2;
        } else if (col_type == GE_VTYPE_COL_5551) {
            uint16_t val = read_u16(rdram, base + offset);
            v.color[0] = static_cast<uint8_t>(
                ((val & 0x1F) * 255) / 31);
            v.color[1] = static_cast<uint8_t>(
                (((val >> 5) & 0x1F) * 255) / 31);
            v.color[2] = static_cast<uint8_t>(
                (((val >> 10) & 0x1F) * 255) / 31);
            v.color[3] = (val & 0x8000) ? 255 : 0;
            offset += 2;
        } else if (col_type == GE_VTYPE_COL_4444) {
            uint16_t val = read_u16(rdram, base + offset);
            v.color[0] = static_cast<uint8_t>(
                ((val & 0xF) * 255) / 15);
            v.color[1] = static_cast<uint8_t>(
                (((val >> 4) & 0xF) * 255) / 15);
            v.color[2] = static_cast<uint8_t>(
                (((val >> 8) & 0xF) * 255) / 15);
            v.color[3] = static_cast<uint8_t>(
                (((val >> 12) & 0xF) * 255) / 15);
            offset += 2;
        } else if (col_type == GE_VTYPE_COL_8888) {
            v.color[0] = read_u8(rdram, base + offset);
            v.color[1] = read_u8(rdram, base + offset + 1);
            v.color[2] = read_u8(rdram, base + offset + 2);
            v.color[3] = read_u8(rdram, base + offset + 3);
            offset += 4;
        } else {
            // No vertex colour: the PSP uses the material ambient colour
            // (GE_CMD_MATERIALAMBIENT, 0xBBGGRR) with GE_CMD_MATERIALALPHA —
            // this is how untextured fade quads get their colour and alpha.
            v.color[0] = static_cast<uint8_t>(state.material_ambient & 0xFF);
            v.color[1] = static_cast<uint8_t>((state.material_ambient >> 8) & 0xFF);
            v.color[2] = static_cast<uint8_t>((state.material_ambient >> 16) & 0xFF);
            v.color[3] = static_cast<uint8_t>(state.material_alpha & 0xFF);
        }

        // Normal
        offset = L.nrm_off;
        if (nrm_type == GE_VTYPE_NRM_8BIT) {
            v.normal[0] = read_s8(rdram, base + offset)
                          / 127.0f;
            v.normal[1] = read_s8(rdram, base + offset + 1)
                          / 127.0f;
            v.normal[2] = read_s8(rdram, base + offset + 2)
                          / 127.0f;
            offset += 3;
        } else if (nrm_type == GE_VTYPE_NRM_16BIT) {
            v.normal[0] = read_s16(rdram, base + offset)
                          / 32767.0f;
            v.normal[1] = read_s16(rdram, base + offset + 2)
                          / 32767.0f;
            v.normal[2] = read_s16(rdram, base + offset + 4)
                          / 32767.0f;
            offset += 6;
        } else if (nrm_type == GE_VTYPE_NRM_FLOAT) {
            v.normal[0] = read_float(rdram, base + offset);
            v.normal[1] = read_float(rdram,
                                     base + offset + 4);
            v.normal[2] = read_float(rdram,
                                     base + offset + 8);
            offset += 12;
        } else {
            v.normal[0] = 0.0f;
            v.normal[1] = 0.0f;
            v.normal[2] = 1.0f;
        }

        // Position
        offset = L.pos_off;
        if (pos_type == GE_VTYPE_POS_8BIT) {
            // Through: raw pixels; otherwise fixed point /128.
            const float k = through ? 1.0f : (1.0f / 128.0f);
            v.pos[0] = read_s8(rdram, base + offset) * k;
            v.pos[1] = read_s8(rdram, base + offset + 1) * k;
            v.pos[2] = through ? static_cast<float>(read_u8(rdram, base + offset + 2))
                               : read_s8(rdram, base + offset + 2) * k;
        } else if (pos_type == GE_VTYPE_POS_16BIT) {
            // Through: raw pixels (z unsigned); otherwise fixed point /32768.
            const float k = through ? 1.0f : (1.0f / 32768.0f);
            v.pos[0] = read_s16(rdram, base + offset) * k;
            v.pos[1] = read_s16(rdram, base + offset + 2) * k;
            v.pos[2] = through ? static_cast<float>(read_u16(rdram, base + offset + 4))
                               : read_s16(rdram, base + offset + 4) * k;
        } else if (pos_type == GE_VTYPE_POS_FLOAT) {
            v.pos[0] = read_float(rdram, base + offset);
            v.pos[1] = read_float(rdram,
                                  base + offset + 4);
            v.pos[2] = read_float(rdram,
                                  base + offset + 8);
        } else {
            v.pos[0] = 0.0f;
            v.pos[1] = 0.0f;
            v.pos[2] = 0.0f;
        }

        // Skinning: sum_j w_j * (Bone_j * pos) (PPSSPP Step_WeightsU8 +
        // skinning in the software transform). Applied before the world
        // matrix; bone matrices are 4x3 column-major like the world matrix.
        if (L.wt != 0 && !through) {
            float p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
            for (int k = 0; k < wn; k++) {
                const float w = weights[k];
                if (w == 0.0f) continue;
                const float* m = &state.bone_matrix[k * 12];
                float tp[3];
                vec3_by_matrix43(m, v.pos, tp);
                p[0] += w * tp[0]; p[1] += w * tp[1]; p[2] += w * tp[2];
                if (v.has_normal) {
                    n[0] += w * (v.normal[0] * m[0] + v.normal[1] * m[3] + v.normal[2] * m[6]);
                    n[1] += w * (v.normal[0] * m[1] + v.normal[1] * m[4] + v.normal[2] * m[7]);
                    n[2] += w * (v.normal[0] * m[2] + v.normal[1] * m[5] + v.normal[2] * m[8]);
                }
            }
            v.pos[0] = p[0]; v.pos[1] = p[1]; v.pos[2] = p[2];
            if (v.has_normal) { v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2]; }
        }
    }

    (void)prim_type;  // Used by caller for primitive assembly
}

// ---- Transform ----

// Degenerate-matrix detection. These compensate for
// guest-side broken matrix uploads (open issue, FPU/VFPU
// dataflow family): view arrives all-zero and proj arrives
// NaN/Inf or with a collapsed diagonal. Remove the fallbacks
// once the guest uploads sane matrices.

/// True if every element of the view matrix is zero.
static bool ge_view_matrix_all_zero(const GeState& state) {
    for (int i = 0; i < 12; i++) {
        if (state.view_matrix[i] != 0.0f) return false;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
            "[GE] view matrix all-zero -- NDC-direct "
            "fallback engaged\n");
    }
    return true;
}

/// True if the proj matrix is non-finite or has a
/// collapsed X/Y diagonal (column-major indices 0 and 5).
static bool ge_proj_matrix_degenerate(const GeState& state) {
    bool bad = std::fabs(state.proj_matrix[0]) < 1e-6f
            || std::fabs(state.proj_matrix[5]) < 1e-6f;
    for (int i = 0; i < 16 && !bad; i++) {
        if (!std::isfinite(state.proj_matrix[i])) bad = true;
    }
    if (bad) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE] proj matrix degenerate -- NDC-direct "
                "fallback engaged\n");
        }
    }
    return bad;
}

/// Compose model->world->view->proj for one position and return its NDC
/// (post perspective-divide). Mirrors the real transform path below.
static void ge_compose_mvp_ndc(
    const GeState& state, const float pos[3], float ndc[3]
) {
    float wpos[3], vpos[3], clip[4];
    vec3_by_matrix43(state.world_matrix, pos, wpos);
    vec3_by_matrix43(state.view_matrix, wpos, vpos);
    vec3_by_matrix44(state.proj_matrix, vpos, clip);
    if (std::fabs(clip[3]) > 1e-6f) {
        ndc[0] = clip[0] / clip[3];
        ndc[1] = clip[1] / clip[3];
        ndc[2] = clip[2] / clip[3];
    } else {
        ndc[0] = clip[0];
        ndc[1] = clip[1];
        ndc[2] = clip[2];
    }
}

/// Generic collapsed-MVP detector. The real transform inputs (guest-uploaded
/// matrices) are still wrong (FPU/VFPU dataflow family, open issue): they
/// now arrive non-zero but numerically broken, so neither the all-zero nor
/// the diagonal-degenerate gate above fires, yet the composed MVP crushes a
/// whole primitive onto a few-pixel sliver at screen center -- a black screen.
///
/// Detection is GENERIC (no game constants). It transforms the prim's
/// vertices through the real MVP and compares the post-transform NDC footprint
/// against the pre-transform model-space footprint, using two independent
/// signals that must BOTH hold so legitimately-small geometry is never
/// mis-flagged:
///
///   1. ABSOLUTE: the prim's NDC footprint is below a few-pixel "effectively
///      invisible" ceiling (NDC_INVISIBLE_MAX). A correctly-rendered visible
///      element is larger than this.
///   2. RELATIVE: the shrink ratio (NDC extent per unit of model spread) is
///      far below any plausible projection (RATIO_MIN). A genuinely-small but
///      correctly-transformed element keeps a normal ratio (its small NDC
///      matches its small model), so it fails this and is left alone; only a
///      meaningfully-spread prim that the MVP crushes to a sliver fails both.
///
/// A non-finite composed NDC always counts as collapsed. Measured values:
/// broken Patapon title prims read model spread ~28 -> NDC extent ~0.02
/// (ratio ~0.0007); a healthy 1:1 ortho maps 480 model units -> NDC 2.0
/// (ratio ~0.0042) and never trips the absolute ceiling.
static bool ge_mvp_collapses(
    const GeState& state, const std::vector<DecodedVertex>& verts
) {
    if (verts.size() < 2) return false;

    // ~8 px on a 480-wide target (8 * 2/480). A real visible element spans
    // more; a crushed prim is far below this.
    constexpr float NDC_INVISIBLE_MAX = 8.0f * 2.0f / 480.0f;  // ~0.0333
    // Minimum plausible NDC-per-model-unit. A healthy 1:1 ortho is ~0.0042;
    // the broken title is ~0.0007. 0.002 sits ~3x under healthy and ~3x over
    // broken -- a wide guard band either way.
    constexpr float RATIO_MIN = 0.002f;
    // Below this model spread the input is genuinely tiny, so a small NDC is
    // correct (the ratio test would be noisy on a near-zero denominator).
    constexpr float MODEL_SPREAD_MIN = 4.0f;

    float ndc_min[2] = {1e30f, 1e30f};
    float ndc_max[2] = {-1e30f, -1e30f};
    float mdl_min[3] = {1e30f, 1e30f, 1e30f};
    float mdl_max[3] = {-1e30f, -1e30f, -1e30f};
    bool non_finite = false;

    for (const auto& v : verts) {
        float ndc[3];
        ge_compose_mvp_ndc(state, v.pos, ndc);
        for (int k = 0; k < 2; k++) {
            if (!std::isfinite(ndc[k])) { non_finite = true; continue; }
            if (ndc[k] < ndc_min[k]) ndc_min[k] = ndc[k];
            if (ndc[k] > ndc_max[k]) ndc_max[k] = ndc[k];
        }
        for (int k = 0; k < 3; k++) {
            if (v.pos[k] < mdl_min[k]) mdl_min[k] = v.pos[k];
            if (v.pos[k] > mdl_max[k]) mdl_max[k] = v.pos[k];
        }
    }

    float mdl_spread = 0.0f;
    for (int k = 0; k < 3; k++)
        mdl_spread = std::max(mdl_spread, mdl_max[k] - mdl_min[k]);
    float ndc_extent = std::max(ndc_max[0] - ndc_min[0],
                                ndc_max[1] - ndc_min[1]);
    float ratio = (mdl_spread > 1e-6f) ? (ndc_extent / mdl_spread) : 1.0f;

    bool collapsed =
        non_finite
        || (mdl_spread > MODEL_SPREAD_MIN
            && ndc_extent < NDC_INVISIBLE_MAX
            && ratio < RATIO_MIN);
    if (collapsed) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE] composed MVP collapses prim (model spread %.3f -> "
                "NDC extent %.5f, ratio %.5f) -- NDC-direct fallback "
                "engaged\n", mdl_spread, ndc_extent, ratio);
        }
    }
    return collapsed;
}

// Game-module degenerate-matrix fallback slot (#47 P5 seam).
static GeDegenerateFallbackFn g_degenerate_fallback = nullptr;

void ge_vertex_set_degenerate_fallback(GeDegenerateFallbackFn fn) {
    g_degenerate_fallback = fn;
}

// ---- Lighting (per-vertex, PSP GE fixed function; PPSSPP VertexShaderGenerator) ----
//
// Lit colour = emissive + ambientGlobal * matAmbient
//            + sum_i att_i * (lightAmb_i * matAmbient + lightDif_i * matDiffuse * max(N.L_i, 0))
// where matAmbient / matDiffuse come from the vertex colour when the matching
// MATERIALUPDATE bit is set, else from the MATERIAL* registers. Alpha is
// ambientAlpha * matAmbientAlpha. Specular is not modelled (rare; would need a
// second colour output). All lights/normals are in world space. Without this a
// title that runs the GE "lit" with a 0.5 global ambient and doubles texture
// output (Square Enix's 0x80 == 1.0 convention) renders every surface 2x too
// bright (saturated white).
static inline float rgb_c(uint32_t c, int ch) {
    return static_cast<float>((c >> (ch * 8)) & 0xFFu) * (1.0f / 255.0f);
}

static void ge_apply_lighting(const GeState& state, DecodedVertex& v,
                              const float wpos[3]) {
    const bool upd_amb = (state.material_update & 1u) && v.has_color;
    const bool upd_dif = (state.material_update & 2u) && v.has_color;
    float vc[4] = {v.color[0] / 255.0f, v.color[1] / 255.0f,
                   v.color[2] / 255.0f, v.color[3] / 255.0f};
    float mamb[4], mdif[3];
    for (int c = 0; c < 3; c++) {
        mamb[c] = upd_amb ? vc[c] : rgb_c(state.material_ambient, c);
        mdif[c] = upd_dif ? vc[c] : rgb_c(state.material_diffuse, c);
    }
    mamb[3] = upd_amb ? vc[3] : static_cast<float>(state.material_alpha & 0xFFu) / 255.0f;

    float out[3];
    for (int c = 0; c < 3; c++)
        out[c] = rgb_c(state.material_emissive, c)
               + rgb_c(state.ambient_color, c) * mamb[c];
    float alpha = (static_cast<float>(state.ambient_alpha & 0xFFu) / 255.0f) * mamb[3];

    // World-space normal (transform by the world matrix, no translation).
    float n[3] = {0, 0, 0};
    const bool have_n = v.has_normal;
    if (have_n) {
        const float* m = state.world_matrix;
        n[0] = v.normal[0] * m[0] + v.normal[1] * m[3] + v.normal[2] * m[6];
        n[1] = v.normal[0] * m[1] + v.normal[1] * m[4] + v.normal[2] * m[7];
        n[2] = v.normal[0] * m[2] + v.normal[1] * m[5] + v.normal[2] * m[8];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }

    for (int i = 0; i < 4; i++) {
        if (!state.light_enable[i]) continue;
        const int kind = (state.light_type[i] >> 8) & 3;
        float L[3];
        float scale = 1.0f;
        if (kind == 0) {  // directional: position holds the direction to the light
            L[0] = state.light_pos[i][0]; L[1] = state.light_pos[i][1]; L[2] = state.light_pos[i][2];
            const float len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (len > 1e-8f) { L[0] /= len; L[1] /= len; L[2] /= len; }
        } else {
            L[0] = state.light_pos[i][0] - wpos[0];
            L[1] = state.light_pos[i][1] - wpos[1];
            L[2] = state.light_pos[i][2] - wpos[2];
            const float d = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (d > 1e-8f) { L[0] /= d; L[1] /= d; L[2] /= d; }
            const float att = state.light_att[i][0] + state.light_att[i][1] * d
                            + state.light_att[i][2] * d * d;
            scale = att > 1e-8f ? std::min(1.0f / att, 1.0f) : 1.0f;
            if (kind == 2) {  // spot
                const float* D = state.light_dir[i];
                const float dl = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
                float ang = 0.0f;
                if (dl > 1e-8f)
                    ang = -(L[0] * D[0] + L[1] * D[1] + L[2] * D[2]) / dl;
                if (ang >= state.light_spot_cut[i])
                    scale *= std::pow(std::max(ang, 0.0f), state.light_spot_exp[i]);
                else
                    scale = 0.0f;
            }
        }
        const float dot = have_n
            ? std::max(n[0] * L[0] + n[1] * L[1] + n[2] * L[2], 0.0f) : 0.0f;
        for (int c = 0; c < 3; c++) {
            out[c] += scale * (rgb_c(state.light_amb[i], c) * mamb[c]
                             + rgb_c(state.light_dif[i], c) * mdif[c] * dot);
        }
    }
    for (int c = 0; c < 3; c++)
        v.color[c] = static_cast<uint8_t>(std::min(std::max(out[c], 0.0f), 1.0f) * 255.0f + 0.5f);
    v.color[3] = static_cast<uint8_t>(std::min(std::max(alpha, 0.0f), 1.0f) * 255.0f + 0.5f);
}

void ge_transform_vertices(
    std::vector<DecodedVertex>& verts,
    const GeState& state
) {
    bool through = ge_vtype_through(state.vertex_type);

    if (through) {
        // Through-mode: map screen coords to NDC [-1,1]
        // PSP screen is 480x272
        //
        // Through-mode UVs arrive in TEXEL units (e.g. 64.0 for a
        // 128-wide texture), but GL samples in normalized [0,1]
        // coords. PPSSPP normalizes by the texture dimensions
        // (GPU/Common/SoftwareTransformCommon.cpp: uscale /=
        // curTextureWidth; vscale /= curTextureHeight). Without
        // this, every content UV >= 1.0 clamps to the bottom-right
        // texel under GL_CLAMP_TO_EDGE -> textured 2D content
        // discards. tex_size[0] packs log2 dims: log2_w | (log2_h<<8).
        const float texw = static_cast<float>(
            1u << (state.tex_size[0] & 0xFFu));
        const float texh = static_cast<float>(
            1u << ((state.tex_size[0] >> 8) & 0xFFu));
        for (auto& v : verts) {
            v.pos[0] = v.pos[0] / 240.0f - 1.0f;
            v.pos[1] = 1.0f - v.pos[1] / 136.0f;
            v.pos[2] = v.pos[2] / 65535.0f;
            v.w = 1.0f;   // already NDC: no perspective divide (see ge_decode_vertices)

            // Apply tex scale/offset, then texel -> [0,1] normalize.
            if (v.has_uv) {
                v.uv[0] = v.uv[0] * state.tex_scale_u
                          + state.tex_offset_u;
                v.uv[1] = v.uv[1] * state.tex_scale_v
                          + state.tex_offset_v;
                if (state.texture_enable) {
                    if (texw > 0.0f) v.uv[0] /= texw;
                    if (texh > 0.0f) v.uv[1] /= texh;
                }
            }
        }
    } else {
        // Transform mode: model -> world -> view -> proj
        // using PSP column-major matrix semantics.
        //
        // FALLBACKS: the guest currently uploads broken
        // matrices (view all-zero; proj NaN/Inf or malformed
        // diagonal) -- open issue, FPU/VFPU dataflow family.
        // Remove these workarounds when that is fixed.
        //
        // An all-zero view with a valid proj can never render
        // correctly through the real path (identity-view is a
        // wrong guess against the title's ortho extents), so an
        // all-zero view ALSO routes to the degenerate fallback.
        // The full real path engages automatically once the guest
        // uploads a non-zero view matrix.
        // The all-zero / diagonal-degenerate gates catch the matrices that
        // are *obviously* broken. ge_mvp_collapses additionally catches the
        // subtler case (#27 family): matrices that arrive non-zero and
        // non-singular but compose into an MVP that maps the whole prim onto
        // a sub-pixel cluster -- a silent black screen the static gates miss.
        bool view_zero = ge_view_matrix_all_zero(state);
        bool proj_bad = ge_proj_matrix_degenerate(state);
        bool mvp_collapsed =
            !view_zero && !proj_bad && ge_mvp_collapses(state, verts);
        bool ndc_direct = view_zero || proj_bad || mvp_collapsed;

        for (auto& v : verts) {
            float wpos[3], vpos[3];
            vec3_by_matrix43(state.world_matrix,
                             v.pos, wpos);
            if (state.lighting_enable) ge_apply_lighting(state, v, wpos);
            if (!ndc_direct) {
                vec3_by_matrix43(state.view_matrix,
                                 wpos, vpos);
            }

            if (ndc_direct) {
                // Bypass proj: emit NDC directly from world-space.
                // The mapping is title-tuned and installs from the
                // game module (#47 P5 seam); without one, pass the
                // world-space position through unchanged.
                if (g_degenerate_fallback != nullptr) {
                    g_degenerate_fallback(wpos, v.pos);
                } else {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        std::fprintf(stderr,
                            "[GE] degenerate-matrix fallback: no game "
                            "fallback installed -- world-space "
                            "passthrough\n");
                    }
                    v.pos[0] = wpos[0];
                    v.pos[1] = wpos[1];
                    v.pos[2] = wpos[2];
                }
                v.w = 1.0f;   // NDC-direct: no perspective divide
            } else {
                float clip[4];
                vec3_by_matrix44(state.proj_matrix,
                                 vpos, clip);

                // Leave in clip space: the GPU does the perspective divide,
                // near/far clipping and perspective-correct interpolation.
                v.pos[0] = clip[0];
                v.pos[1] = clip[1];
                v.pos[2] = clip[2];
                v.w = clip[3];
            }

            // Apply tex scale/offset
            if (v.has_uv) {
                v.uv[0] = v.uv[0] * state.tex_scale_u
                          + state.tex_offset_u;
                v.uv[1] = v.uv[1] * state.tex_scale_v
                          + state.tex_offset_v;
            }
        }
    }
}


// ---- GPU-path decode ----

bool ge_gpu_transform_ok(const GeState& state) {
    if (g_degenerate_fallback != nullptr) return false;
    return !ge_view_matrix_all_zero(state) && !ge_proj_matrix_degenerate(state);
}

template <typename T>
static inline T ld(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

bool ge_decode_vertices_gpu(
    uint8_t* rdram, const GeState& state, int count, GeGpuVertex* out
) {
    const uint32_t vtype = state.vertex_type;
    const VtxLayout L = vtx_layout(vtype);
    const int stride = L.stride;
    if (stride <= 0 || count <= 0) return false;
    const int tc_type = ge_vtype_tc(vtype);
    const int col_type = ge_vtype_col(vtype);
    const int nrm_type = ge_vtype_nrm(vtype);
    const int pos_type = ge_vtype_pos(vtype);
    const int idx_type = ge_vtype_idx(vtype);

    auto range_ok = [](uint32_t addr, uint64_t bytes) {
        if (addr < 0x00010000U) return false;
        return (uint64_t)(addr & 0x07FFFFFFU) + bytes <= 0x08000000ULL;
    };

    // Indices.
    static thread_local std::vector<uint32_t> indices;
    uint32_t max_idx = static_cast<uint32_t>(count - 1);
    if (idx_type != GE_VTYPE_IDX_NONE) {
        const uint32_t isz = idx_type == GE_VTYPE_IDX_8BIT ? 1u
                           : idx_type == GE_VTYPE_IDX_16BIT ? 2u : 4u;
        if (!range_ok(state.index_addr, (uint64_t)count * isz)) return false;
        const uint8_t* ip = rdram + (state.index_addr & 0x07FFFFFFU);
        indices.resize(count);
        max_idx = 0;
        for (int i = 0; i < count; i++) {
            uint32_t v = isz == 1 ? ip[i] : isz == 2 ? ld<uint16_t>(ip + i * 2)
                                                     : ld<uint32_t>(ip + i * 4);
            indices[i] = v;
            if (v > max_idx) max_idx = v;
        }
    }
    if (!range_ok(state.vertex_addr, (uint64_t)(max_idx + 1) * stride)) return false;
    const uint8_t* vb = rdram + (state.vertex_addr & 0x07FFFFFFU);

    const int wn = std::min(L.wcount, 8);
    const bool skinned = L.wt != 0;

    // Constant colour for colourless vertices: material ambient + alpha.
    uint8_t def_col[4] = {
        static_cast<uint8_t>(state.material_ambient & 0xFF),
        static_cast<uint8_t>((state.material_ambient >> 8) & 0xFF),
        static_cast<uint8_t>((state.material_ambient >> 16) & 0xFF),
        static_cast<uint8_t>(state.material_alpha & 0xFF)};

    for (int i = 0; i < count; i++) {
        const uint32_t vidx = (idx_type != GE_VTYPE_IDX_NONE) ? indices[i]
                                                              : static_cast<uint32_t>(i);
        const uint8_t* p = vb + (size_t)vidx * stride;
        GeGpuVertex& v = out[i];

        if (skinned) {
            const uint8_t* wp = p + L.weight_off;
            if (L.wt == 1) {
                for (int k = 0; k < wn; k++) v.weights[k] = wp[k] / 128.0f;
            } else if (L.wt == 2) {
                for (int k = 0; k < wn; k++) v.weights[k] = ld<uint16_t>(wp + k * 2) / 32768.0f;
            } else {
                for (int k = 0; k < wn; k++) v.weights[k] = ld<float>(wp + k * 4);
            }
            for (int k = wn; k < 8; k++) v.weights[k] = 0.0f;
        }

        if (tc_type == GE_VTYPE_TC_8BIT) {
            v.uv[0] = static_cast<float>(p[L.tc_off]) / 128.0f;
            v.uv[1] = static_cast<float>(p[L.tc_off + 1]) / 128.0f;
        } else if (tc_type == GE_VTYPE_TC_16BIT) {
            v.uv[0] = static_cast<float>(ld<uint16_t>(p + L.tc_off)) / 32768.0f;
            v.uv[1] = static_cast<float>(ld<uint16_t>(p + L.tc_off + 2)) / 32768.0f;
        } else if (tc_type == GE_VTYPE_TC_FLOAT) {
            v.uv[0] = ld<float>(p + L.tc_off);
            v.uv[1] = ld<float>(p + L.tc_off + 4);
        } else {
            v.uv[0] = 0.0f;
            v.uv[1] = 0.0f;
        }

        if (col_type == GE_VTYPE_COL_565) {
            const uint16_t val = ld<uint16_t>(p + L.col_off);
            v.color[0] = static_cast<uint8_t>(((val & 0x1F) * 255) / 31);
            v.color[1] = static_cast<uint8_t>((((val >> 5) & 0x3F) * 255) / 63);
            v.color[2] = static_cast<uint8_t>((((val >> 11) & 0x1F) * 255) / 31);
            v.color[3] = 255;
        } else if (col_type == GE_VTYPE_COL_5551) {
            const uint16_t val = ld<uint16_t>(p + L.col_off);
            v.color[0] = static_cast<uint8_t>(((val & 0x1F) * 255) / 31);
            v.color[1] = static_cast<uint8_t>((((val >> 5) & 0x1F) * 255) / 31);
            v.color[2] = static_cast<uint8_t>((((val >> 10) & 0x1F) * 255) / 31);
            v.color[3] = (val & 0x8000) ? 255 : 0;
        } else if (col_type == GE_VTYPE_COL_4444) {
            const uint16_t val = ld<uint16_t>(p + L.col_off);
            v.color[0] = static_cast<uint8_t>(((val & 0xF) * 255) / 15);
            v.color[1] = static_cast<uint8_t>((((val >> 4) & 0xF) * 255) / 15);
            v.color[2] = static_cast<uint8_t>((((val >> 8) & 0xF) * 255) / 15);
            v.color[3] = static_cast<uint8_t>((((val >> 12) & 0xF) * 255) / 15);
        } else if (col_type == GE_VTYPE_COL_8888) {
            std::memcpy(v.color, p + L.col_off, 4);
        } else {
            std::memcpy(v.color, def_col, 4);
        }

        if (nrm_type == GE_VTYPE_NRM_8BIT) {
            v.normal[0] = static_cast<int8_t>(p[L.nrm_off]) / 127.0f;
            v.normal[1] = static_cast<int8_t>(p[L.nrm_off + 1]) / 127.0f;
            v.normal[2] = static_cast<int8_t>(p[L.nrm_off + 2]) / 127.0f;
        } else if (nrm_type == GE_VTYPE_NRM_16BIT) {
            v.normal[0] = ld<int16_t>(p + L.nrm_off) / 32767.0f;
            v.normal[1] = ld<int16_t>(p + L.nrm_off + 2) / 32767.0f;
            v.normal[2] = ld<int16_t>(p + L.nrm_off + 4) / 32767.0f;
        } else if (nrm_type == GE_VTYPE_NRM_FLOAT) {
            v.normal[0] = ld<float>(p + L.nrm_off);
            v.normal[1] = ld<float>(p + L.nrm_off + 4);
            v.normal[2] = ld<float>(p + L.nrm_off + 8);
        } else {
            v.normal[0] = 0.0f;
            v.normal[1] = 0.0f;
            v.normal[2] = 1.0f;
        }

        if (pos_type == GE_VTYPE_POS_8BIT) {
            const float k = 1.0f / 128.0f;
            v.pos[0] = static_cast<int8_t>(p[L.pos_off]) * k;
            v.pos[1] = static_cast<int8_t>(p[L.pos_off + 1]) * k;
            v.pos[2] = static_cast<int8_t>(p[L.pos_off + 2]) * k;
        } else if (pos_type == GE_VTYPE_POS_16BIT) {
            const float k = 1.0f / 32768.0f;
            v.pos[0] = ld<int16_t>(p + L.pos_off) * k;
            v.pos[1] = ld<int16_t>(p + L.pos_off + 2) * k;
            v.pos[2] = ld<int16_t>(p + L.pos_off + 4) * k;
        } else if (pos_type == GE_VTYPE_POS_FLOAT) {
            v.pos[0] = ld<float>(p + L.pos_off);
            v.pos[1] = ld<float>(p + L.pos_off + 4);
            v.pos[2] = ld<float>(p + L.pos_off + 8);
        } else {
            v.pos[0] = v.pos[1] = v.pos[2] = 0.0f;
        }
    }
    return true;
}
