// Shared by the post-processing passes over the HDR scene target.
#ifdef WZ_DEPTH_MS
layout(set = 0, binding = 0) uniform sampler2DMS scene_depth;
float wzDepth(ivec2 pixel) { return texelFetch(scene_depth, pixel, 0).r; }
#else
layout(set = 0, binding = 0) uniform sampler2D scene_depth;
float wzDepth(ivec2 pixel) { return texelFetch(scene_depth, pixel, 0).r; }
#endif

layout(push_constant) uniform PostParameters {
    mat4 projection;  // the world camera's Vulkan projection (reverse Z)
    ivec4 rect;       // the world viewport: x, y, width, height in pixels
    vec4 parameters;  // pass-specific
} post;

// Eye-space position of a pixel inside the world viewport.
vec3 wzEyePosition(ivec2 pixel, float depth, mat4 inverse_projection)
{
    vec2 ndc = (vec2(pixel - post.rect.xy) + 0.5) / vec2(post.rect.zw) * 2.0 - 1.0;
    vec4 eye = inverse_projection * vec4(ndc, depth, 1.0);
    return eye.xyz / eye.w;
}

bool wzInside(ivec2 pixel)
{
    return all(greaterThanEqual(pixel, post.rect.xy)) &&
           all(lessThan(pixel, post.rect.xy + post.rect.zw));
}
