#pragma once
#include "psp_ge.h"
#include <glad/glad.h>

/// Shader uniform locations cache.
struct ShaderUniforms {
    GLint u_texture_enable;
    GLint u_texture;
    GLint u_tex_func;
    GLint u_tex_rgba;
    GLint u_tex_double;
    GLint u_tex_env;
    GLint u_alpha_test_enable;
    GLint u_alpha_test_ref;
    GLint u_alpha_test_func;
    GLint u_alpha_double;
    GLint u_alpha_override;
    GLint u_tex_remap;
    GLint u_alpha_test_mask;
    GLint u_color_test_enable;
    GLint u_color_test_func;
    GLint u_color_test_ref;
    GLint u_color_test_mask;
    // Vertex-stage GE transform (psp_ge_shader.cpp)
    GLint u_xform, u_world, u_view, u_proj, u_uvxf, u_skin, u_bones;
    GLint u_lit, u_lflags, u_nl, u_lc, u_lt;
};

/// Per-draw inputs of the vertex-shader GE transform path (u_xform = 1).
struct GeXform3D {
    const float* world;     // 4x3 column-major
    const float* view;      // 4x3 column-major
    const float* proj;      // 4x4 column-major
    float uvxf[4];          // tex scale u,v / offset u,v
    int skin_n;             // weights used (0 = unskinned)
    const float* bones;     // skin_n 4x3 matrices
    int lit;
    int lflags;             // bit0 amb<-vtx colour, bit1 dif<-vtx colour, bit2 has normal
    int nlights;            // enabled lights packed first
    float lc[4][4];
    float lt[20][4];
};

/// Vertex stage passes a_position through (CPU-transformed vertices).
void ge_shader_set_xform_passthrough();

/// Vertex stage runs the GE transform pipeline on model-space vertices.
void ge_shader_set_xform_3d(const GeXform3D& x);

/// Forget cached uniform values (after anything else touched the program).
void ge_shader_invalidate_cache();

/// Compile and link the uber-shader pair. Call after GL init.
void ge_shader_init();

/// Delete shader program.
void ge_shader_shutdown();

/// Bind the shader program.
void ge_shader_use();

/// Update all uniforms from current GE state.
void ge_shader_set_uniforms(const GeState& state);

/// Per-draw: double (and clamp) the output fragment alpha, used to emulate
/// the GE's 2*src-alpha blend factors. Call after ge_shader_set_uniforms.
void ge_shader_set_alpha_double(bool enable);

/// Per-draw: replace the fragment alpha by `a` (0..1) when a >= 0 (GE stencil
/// REPLACE / ZERO keeps the alpha-resident stencil in step). Pass -1 for none.
void ge_shader_set_alpha_override(float a);

/// Per-draw UV remap applied after normalisation (FBO-as-texture):
/// s' = s*sx, t' = flip ? 1 - t*sy : t. Identity is (1, 1, false).
void ge_shader_set_tex_remap(float sx, float sy, bool flip);

/// Get the shader program ID (for external uniform queries).
GLuint ge_shader_get_program();

// ---- Shared with the FX shader overrides (psp_fx_shader.cpp) ----

/// The uber vertex shader: "#version ..." line and the body. Compiling the body
/// with "#define PSP_FX 1" in between adds the v_fx_* outputs.
const char* ge_shader_vertex_version();
const char* ge_shader_vertex_body();

/// Look up the uber-shader's uniform names in `prog` (missing ones are -1).
void ge_shader_query_locations(GLuint prog, ShaderUniforms* u);

/// Another program was bound: the next ge_shader_use() must rebind the uber-shader.
void ge_shader_note_external_program();

/// Upload the GE texture-function and vertex-transform uniforms (uncached) to
/// the currently bound program; x == null selects the passthrough path.
void ge_shader_upload_to(const ShaderUniforms& u, const GeState& state, const GeXform3D* x,
                         bool alpha_double);
