#version 450
#extension GL_GOOGLE_include_directive : require

#include "world_blocks.glsl"

layout(set = 3, binding = 1) uniform sampler2D material_texture;
layout(set = 3, binding = 2) uniform sampler2D detail_texture;

#include "world_host.glsl"

#include "wizardry_fragment.glsl"
#ifdef WZ_PER_PIXEL
#include "wizardry_vertex.glsl"
#include "world_shadow.glsl"
#endif

layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec4 v_specular;
layout(location = 2) in vec2 v_st0;
layout(location = 3) in vec3 v_eye;
layout(location = 4) in vec2 v_st1;
#ifdef WZ_PER_PIXEL
layout(location = 5) in vec3 v_normal;
layout(location = 6) flat in vec4 v_object;
layout(location = 7) flat in float v_material;
layout(location = 8) in float v_alpha;
layout(location = 9) in vec4 v_color;
layout(location = 10) in vec4 v_diffuse_multiplier;
layout(location = 11) in vec4 v_specular_multiplier;
#endif
layout(location = 0) out vec4 out_color;
// Alpha-tested cutouts write coverage from their filtered alpha when the pass
// is multisampled, which antialiases foliage and fences.
layout(constant_id = 0) const bool wz_alpha_to_coverage = false;
layout(constant_id = 1) const int wz_samples = 1;

void main()
{
    // Writing the mask anywhere makes it the output for every path.
    gl_SampleMask[0] = -1;
    int clip_base = int(view.values[2].w) + int(view.values[2].y) * 2;
    for (int plane = 0; plane < int(view.values[2].z); ++plane)
        if (dot(view.values[clip_base + plane], vec4(v_eye, 1.0)) < 0.0) discard;
#ifdef WZ_PER_PIXEL
    eye_center = v_object.xyz;
    eye_radius = v_object.w;
    bool has_normal = wzDraw(0).z != 0.0;
    vec3 normal = has_normal ? normalize(v_normal) : vec3(0.0, 0.0, -1.0);
    float sun_shadow = wzSunShadow(v_eye, normal, has_normal);
    float shadow_strength = view.values[WZ_SHADOW_BASE].y;
    wz_sky_light = 1.0 - max(shadow_strength, 0.0) * sun_shadow;
    // Torch shadows take a light's baked share out of the vertex colour; a
    // blocker never removes more than the strength allows.
    vec4 vertex_color = v_color;
    float light_strength = view.values[WZ_LIGHT_SHADOW_BASE].y;
    if ((int(wzDraw(0).y) & WZ_HAS_COLORS) != 0) {
        vec3 material_diffuse = wzShading(wzMaterialBase(v_material)).rgb;
        vec3 loss = wzLightShadowLoss(v_eye, normal, has_normal, material_diffuse);
        vertex_color.rgb = max(vertex_color.rgb - loss * abs(light_strength),
                               vertex_color.rgb * (1.0 - abs(light_strength)));
    }
    WzLit lit = wzLight(v_eye, normal, v_material, v_alpha, vertex_color, v_diffuse_multiplier,
                        v_specular_multiplier);
    vec4 color = wzCombine(texture(material_texture, v_st0), lit.diffuse, lit.specular);
#else
    vec4 color = wzCombine(texture(material_texture, v_st0), v_diffuse, v_specular);
#endif
    if (wzAlphaTest()) {
        if (wz_alpha_to_coverage) {
            // Retail keeps every texel with alpha above zero. Fade coverage
            // in over the pixel where filtered alpha crosses zero; constant
            // alpha keeps full coverage.
            float a = color.a;
            float coverage = clamp(a / max(fwidth(a), 1e-6), 0.0, 1.0);
            if (a <= 0.0 || coverage <= 0.0) discard;
            int count = int(coverage * float(wz_samples) + 0.5);
            if (count == 0) discard;
            gl_SampleMask[0] = (1 << count) - 1;
        } else if (color.a <= 0.0) {
            discard;
        }
    }
#ifdef WZ_PER_PIXEL
#endif
#ifdef WZ_PER_PIXEL
    if (shadow_strength < 0.0) color.rgb = vec3(1.0 - sun_shadow);
    if (light_strength < 0.0) color.rgb = vec3(1.0 - wz_debug_light_shadow);
#endif
    out_color = color;
}
