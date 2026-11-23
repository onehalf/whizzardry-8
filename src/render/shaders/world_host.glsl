// Host bindings for the Wizardry shading rules: per-object instance state,
// the pass material table and per-object light/fog predicates.
layout(std140, set = 1, binding = 0) uniform Instance {
    vec4 ambient;
    vec4 bounds;
    vec4 overrides;
    uvec4 masks;
} instance;
layout(std430, set = 2, binding = 0) readonly buffer Pass {
    vec4 values[];
} pass;

vec3 eye_center;
float eye_radius;

vec4 wzShading(int index)
{
    if (index == 0) return instance.ambient;
    if (index == 1) return view.values[0];
    if (index == 2) return view.values[2];
    if (index < WZ_FOG_BASE) return view.values[index];
    if (index < WZ_MATERIAL_BASE) {
        vec4 fog = view.values[WZ_FOG_BASE + (index - WZ_FOG_BASE) * 2];
        float limit = eye_radius + fog.w;
        return vec4(fog.xyz, dot(eye_center, eye_center) <= limit * limit ? 1.0 : 0.0);
    }
    int slot = index - WZ_MATERIAL_BASE;
    vec4 value = pass.values[1 + slot];
    int field = slot % WZ_MATERIAL_VEC4S;
    if (field == 0 && instance.overrides.x != 0.0) value.w *= instance.overrides.y;
    if (field == 3 && instance.overrides.z != 0.0) value = vec4(vec3(instance.overrides.w), 1.0);
    return value;
}

vec4 wzDraw(int index)
{
    if (index == 0) return vec4(batch.channels.x, pass.values[0].xy, 0.0);
    if (index == 2) return batch.modes;
    if (index == 3) return view.values[1];
    if (index == 4) return batch.alpha_test;
    return vec4(0.0);
}

bool wzLightActive(int light)
{
    int base = WZ_LIGHT_BASE + light * WZ_LIGHT_VEC4S;
    if ((floatBitsToUint(view.values[base + 7].x) & instance.masks.x) != 0u) return false;
    vec4 location = view.values[base];
    int flags = int(location.w);
    vec3 offset = eye_center - location.xyz;
    float distance_squared = dot(offset, offset);
    if ((flags & 0x20) != 0) {
        float reach = view.values[base + 5].z + eye_radius;
        if (reach * reach <= distance_squared) return false;
    }
    if ((flags & 0x02) != 0 && eye_radius * eye_radius < distance_squared) {
        vec4 spot = view.values[base + 4];
        float distance = sqrt(distance_squared);
        if (dot(offset, spot.xyz) / distance +
                eye_radius / sqrt(eye_radius * eye_radius + distance_squared) < spot.w)
            return false;
    }
    return true;
}

bool wzFogActive(int layer)
{
    int base = WZ_FOG_BASE + layer * 2;
    if ((floatBitsToUint(view.values[base + 1].x) & instance.masks.x) != 0u) return false;
    vec4 fog = view.values[base];
    if (fog.z <= 0.0) return false;
    float start = fog.x;
    return !(eye_radius <= start &&
             dot(eye_center, eye_center) < (start - eye_radius) * (start - eye_radius));
}
