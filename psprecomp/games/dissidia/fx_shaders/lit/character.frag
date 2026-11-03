// Live lighting for a character body (docs/character-lighting.md, charmod_light.py).
// Cel shading in the style of Dissidia Duellum's models: the body falls into a lit tone and a shadow tone with a crisp
// (anti-aliased) edge between them, the shadow tone is the colour itself, darker and richer (not grey), with a small
// hard highlight on bright parts and a thin rim light round the silhouette. The light follows the camera, so the body
// reads the same from every angle. The game's own colour (painted texture x vertex colour x stage ambient, the GE's
// 2x) stays the base, so stage fades and hit flashes still work. Without vertex normals (a body built before
// lighting, or a CPU-transformed draw) the draw is the game's, unchanged.
uniform vec3 u_key_dir;      // towards the key light, camera space (x right, y up, z towards the viewer), unit
uniform float u_key;         // 0 = the game's flat look, 1 = full cel shading
uniform float u_wrap;        // how far the light wraps round the body (0..1): higher = more of the body in the light
uniform float u_threshold;   // where the shadow starts (0..1, of the wrapped light)
uniform float u_edge;        // softness of the light/shadow edge: 0.02 crisp, 0.5 a smooth gradient
uniform float u_bands;       // tones: 2 lit + shadow, 3 adds a mid tone; 0 = smooth
uniform vec3 u_light;        // tint of the lit tone
uniform vec3 u_shadow;       // tint of the shadow tone (multiplied in)
uniform float u_shadow_sat;  // how much richer (more saturated) the shadow tone is
uniform float u_spec;        // hard highlight strength (bright parts only: hair, metal, skin)
uniform float u_spec_size;   // highlight size (0..1)
uniform float u_rim;         // rim light strength
uniform float u_rim_width;   // rim width (0..1 of the silhouette falloff)
uniform vec3 u_rim_dir;      // which side the rim is on, camera space, unit
uniform vec3 u_rim_colour;

float lit_luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lit_step(float x, float at, float e) { return smoothstep(at - e, at + e, x); }

vec4 fx_shade() {
    vec4 base = fx_ge_color();
    if (u_fx_has_normal == 0 || u_fx_has_world == 0) return base;
    vec3 N = fx_normal();
    if (!gl_FrontFacing) N = -N;
    vec3 V = fx_view_dir();
    vec3 Nv = normalize(mat3(u_fx_view) * N);
    vec3 Vv = normalize(mat3(u_fx_view) * V);

    // light: wrapped lambert, then stepped into tones with a crisp, screen-space anti-aliased edge
    float h = clamp((dot(Nv, u_key_dir) + u_wrap) / (1.0 + u_wrap), 0.0, 1.0);
    float e = max(u_edge, fwidth(h) * 0.75);
    float t;
    if (u_bands >= 2.0) {
        float n = floor(u_bands + 0.5) - 1.0;       // edges between the tones
        t = 0.0;
        for (int k = 0; k < 3; k++) {
            if (float(k) >= n) break;
            t += lit_step(h, u_threshold + float(k) * (1.0 - u_threshold) / n, e);
        }
        t /= n;
    } else {
        t = h;
    }
    vec3 lum = vec3(lit_luma(base.rgb));
    vec3 rich = clamp(mix(lum, base.rgb, 1.0 + u_shadow_sat), 0.0, 1.0);
    vec3 shadowed = rich * u_shadow;
    vec3 lit = base.rgb * u_light;
    vec3 col = mix(base.rgb, mix(shadowed, lit, t), u_key);

    // hard highlight, lit side only, on bright parts (hair, metal, skin): not on dark cloth
    vec3 H = normalize(u_key_dir + Vv);
    float sc = mix(0.995, 0.88, clamp(u_spec_size, 0.0, 1.0));
    float hs = dot(Nv, H);
    float spec = lit_step(hs, sc, max(0.006, fwidth(hs))) * t * smoothstep(0.3, 0.75, lum.r) * u_spec;
    col += u_light * spec * 0.5;

    // rim: a thin light edge round the silhouette, on the rim light's side
    float fres = 1.0 - abs(dot(Nv, Vv));
    float side = clamp(dot(normalize(Nv.xy + 1e-5), normalize(u_rim_dir.xy + 1e-5)) * 0.75 + 0.25, 0.0, 1.0);
    float rw = clamp(u_rim_width, 0.01, 1.0);
    float rim = lit_step(fres, 1.0 - rw, max(0.03, fwidth(fres))) * side * u_rim;
    // the stage's ambient fades darken the game colour (fx_tint 1.0 = normal): the rim fades with them
    float amb = clamp(lit_luma(fx_tint()), 0.0, 1.0);
    col += u_rim_colour * rim * amb * mix(0.45, 1.0, lum.r);
    return vec4(min(col, vec3(1.0)), base.a);
}
