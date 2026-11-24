// Sun shadows from the four-cascade depth atlas (view header block at
// WZ_SHADOW_BASE): count, strength, 1/atlas, fade end; world units per texel;
// sun direction (eye space) and fade start; then one eye-to-atlas matrix per
// cascade. Cascade c covers the atlas quadrant (c % 2, c / 2).
layout(set = 0, binding = 1) uniform sampler2D shadow_atlas;
layout(set = 0, binding = 2) uniform sampler2D light_shadow_atlas;

// 4x4 texels around `uv` with tent weights: a bilinear-filtered 3x3 PCF.
float wzShadowFilter(sampler2D atlas, vec2 uv, float depth, vec2 atlas_texel)
{
    vec2 size = 1.0 / atlas_texel;
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
            vec4 stored = textureGather(atlas, at, 0);
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
        shadow = wzShadowFilter(shadow_atlas, at.xy, at.z - 2e-5, vec2(header.z));
        wz_debug_cascade = cascade;
        break;
    }
    shadow *= 1.0 - smoothstep(sun.w, header.w, distance);
    return max(shadow, self_shadow);
}

// Torch shadows (view block at WZ_LIGHT_SHADOW_BASE): count, strength,
// 1/atlas width and height; eye-to-world rotation; then per light its eye
// position and reach, its baked colour and face texel tangent, and one
// eye-to-atlas matrix per cube face (+x, -x, +y, -y, +z, -z in world axes).
// Returns the baked light the shadowed lights do not deliver to this point:
// the share the vertex-light bake gave it, times how hidden the light is.
float wz_debug_light_shadow = 0.0;
vec3 wzLightShadowLoss(vec3 eye, vec3 normal, bool has_normal, vec3 material_diffuse)
{
    vec4 header = view.values[WZ_LIGHT_SHADOW_BASE];
    int count = int(header.x);
    if (count == 0 || !has_normal) return vec3(0.0);
    mat3 to_world = mat3(view.values[WZ_LIGHT_SHADOW_BASE + 1].xyz,
                         view.values[WZ_LIGHT_SHADOW_BASE + 2].xyz,
                         view.values[WZ_LIGHT_SHADOW_BASE + 3].xyz);
    vec3 loss = vec3(0.0);
    for (int light = 0; light < count; ++light) {
        int base = WZ_LIGHT_SHADOW_BASE + 4 + light * WZ_LIGHT_SHADOW_VEC4S;
        vec4 place = view.values[base];
        vec4 color = view.values[base + 1];
        vec3 offset = eye - place.xyz;
        float distance = length(offset);
        if (distance >= place.w || distance == 0.0) continue;
        // The bake: facing times linear falloff over the light's reach.
        float facing = dot(normal, -offset / distance);
        if (facing <= 0.0) continue;
        vec3 share = color.rgb * material_diffuse * facing * (1.0 - distance / place.w);
        // Offset along the normal by about a texel at this distance.
        vec3 lifted = eye + normal * (distance * color.w * (1.5 + 2.0 * (1.0 - facing)));
        vec3 world = to_world * (lifted - place.xyz);
        vec3 magnitude = abs(world);
        int face = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z
                       ? (world.x >= 0.0 ? 0 : 1)
                       : magnitude.y >= magnitude.z ? (world.y >= 0.0 ? 2 : 3)
                                                    : (world.z >= 0.0 ? 4 : 5);
        int matrix = base + 2 + face * 4;
        vec4 at = mat4(view.values[matrix], view.values[matrix + 1], view.values[matrix + 2],
                       view.values[matrix + 3]) * vec4(lifted, 1.0);
        at.xyz /= at.w;
        float shadow = wzShadowFilter(light_shadow_atlas, at.xy, at.z - 1e-6, header.zw);
        if (max(share.r, max(share.g, share.b)) > 0.001)
            wz_debug_light_shadow = max(wz_debug_light_shadow, shadow);
        loss += share * shadow;
    }
    return loss;
}
