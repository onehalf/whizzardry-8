#version 450
// Resolves the HDR world into the window: ambient occlusion, bloom and a
// tone curve that leaves retail's display-space colours untouched below the
// shoulder and rolls highlights off smoothly instead of clipping them.
layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D ao_texture;
layout(set = 0, binding = 2) uniform sampler2D bloom_texture;
// VSG pushes each view's projection and model-view matrices into bytes 0-127.
layout(push_constant) uniform Parameters {
    layout(offset = 128) vec4 grade;  // x exposure, y ao strength, z bloom intensity, w shoulder start
    vec4 look;   // x saturation, y contrast, z debug view, w 1 when AO is valid
} pc;
layout(location = 0) out vec4 out_color;

vec3 shoulder(vec3 x, float k)
{
    vec3 over = max(x - k, 0.0);
    return min(x, vec3(k)) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k)));
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 size = vec2(textureSize(scene_color, 0));
    vec2 uv = gl_FragCoord.xy / size;
    vec3 color = texelFetch(scene_color, pixel, 0).rgb;
    if (pc.look.w != 0.0) color *= mix(1.0, texelFetch(ao_texture, pixel, 0).r, pc.grade.y);
    if (pc.grade.z > 0.0) color += textureLod(bloom_texture, uv, 0.0).rgb * pc.grade.z;
    color *= pc.grade.x;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = max(mix(vec3(luma), color, pc.look.x), 0.0);
    color = max((color - 0.5) * pc.look.y + 0.5, 0.0);
    color = shoulder(color, pc.grade.w);
    if (pc.look.z == 1.0) color = vec3(texelFetch(ao_texture, pixel, 0).r);
    if (pc.look.z == 2.0) color = textureLod(bloom_texture, uv, 0.0).rgb;
    out_color = vec4(color, 1.0);
}
