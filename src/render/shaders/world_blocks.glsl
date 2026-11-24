// Wizardry material state. Dynamic storage buffers are sized from the actual
// content; light, fog and material counts have no renderer-imposed fixed cap.
layout(std430, set = 0, binding = 0) readonly buffer View {
    vec4 values[];
} view;
layout(std140, set = 3, binding = 0) uniform Batch {
    vec4 channels;
    vec4 modes;
    vec4 alpha_test;
} batch;

#define WZ_SHADOW_BASE 3
#define WZ_LIGHT_SHADOW_BASE 22
#define WZ_LIGHT_SHADOW_VEC4S 26
#define WZ_LIGHT_BASE 130
#define WZ_LIGHT_VEC4S 8
#define WZ_FOG_BASE int(view.values[2].w)
#define WZ_MATERIAL_BASE (WZ_FOG_BASE + int(view.values[2].y))
#define WZ_MATERIAL_VEC4S 6

// Retail clamps lit colours to 1. The HDR scene target keeps light up to this
// ceiling so highlights, overlapping lights and additive effects can bloom.
layout(constant_id = 2) const float wz_light_max = 1.0;
vec4 wzClampLight(vec4 color)
{
    return vec4(clamp(color.rgb, 0.0, wz_light_max), clamp(color.a, 0.0, 1.0));
}
