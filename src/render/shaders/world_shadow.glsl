// Sun shadows from the four-cascade depth atlas (view header block at
// WZ_SHADOW_BASE): count, strength, 1/atlas, fade end; world units per texel;
// sun direction (eye space) and fade start; then one eye-to-atlas matrix per
// cascade. Cascade c covers the atlas quadrant (c % 2, c / 2).
layout(set = 0, binding = 1) uniform sampler2D shadow_atlas;

// 4x4 texels around `uv` with tent weights: a bilinear-filtered 3x3 PCF.
float wzShadowFilter(vec2 uv, float depth, float atlas_texel)
{
    float size = 1.0 / atlas_texel;
    vec2 position = uv * size - 0.5;
    vec2 base = floor(position);
    vec2 f = position - base;
    vec4 wx = vec4(1.0 - f.x, 1.0, 1.0, f.x);
    vec4 wy = vec4(1.0 - f.y, 1.0, 1.0, f.y);
    float lit = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 at = (base + vec2(x * 2, y * 2)) * atlas_texel;
            // Gather order: (0,1), (1,1), (1,0), (0,0).
            vec4 stored = textureGather(shadow_atlas, at, 0);
            vec4 pass = vec4(greaterThanEqual(stored, vec4(depth)));
            int cx = x * 2, cy = y * 2;
            lit += pass.w * wx[cx] * wy[cy] + pass.z * wx[cx + 1] * wy[cy] +
                   pass.x * wx[cx] * wy[cy + 1] + pass.y * wx[cx + 1] * wy[cy + 1];
        }
    }
    return 1.0 - lit / 9.0;
}

// 0 = in full sun, 1 = fully shadowed.
int wz_debug_cascade = -1;
float wzSunShadow(vec3 eye, vec3 normal, bool has_normal)
{
    vec4 header = view.values[WZ_SHADOW_BASE];
    int count = int(header.x);
    if (count == 0) return 0.0;
    vec4 texel_world = view.values[WZ_SHADOW_BASE + 1];
    vec4 sun = view.values[WZ_SHADOW_BASE + 2];
    float facing = has_normal ? dot(normal, sun.xyz) : 1.0;
    // Faces turned from the sun are in their own shadow; this also hides
    // acne where the shadow map meets the terminator.
    float self_shadow = has_normal ? smoothstep(0.0, -0.3, facing) : 0.0;
    float distance = length(eye);
    if (distance >= header.w) return self_shadow;
    vec3 lean = has_normal ? normal * (facing < 0.0 ? -1.0 : 1.0) : sun.xyz;
    float shadow = 0.0;
    for (int cascade = 0; cascade < count; ++cascade) {
        // Offset along the normal by about a texel, more at grazing angles.
        float offset = texel_world[cascade] * (1.0 + 2.0 * (1.0 - clamp(abs(facing), 0.0, 1.0)));
        int base = WZ_SHADOW_BASE + 3 + cascade * 4;
        mat4 to_atlas = mat4(view.values[base], view.values[base + 1], view.values[base + 2],
                             view.values[base + 3]);
        vec3 at = (to_atlas * vec4(eye + lean * offset, 1.0)).xyz;
        vec2 quadrant = vec2(cascade % 2, cascade / 2) * 0.5;
        vec2 local = (at.xy - quadrant) * 2.0;
        float margin = 4.0 * header.z * 2.0;
        if (any(lessThan(local, vec2(margin))) || any(greaterThan(local, vec2(1.0 - margin))) ||
            at.z > 1.0)
            continue;
        shadow = wzShadowFilter(at.xy, at.z - 2e-5, header.z);
        wz_debug_cascade = cascade;
        break;
    }
    shadow *= 1.0 - smoothstep(sun.w, header.w, distance);
    return max(shadow, self_shadow);
}
