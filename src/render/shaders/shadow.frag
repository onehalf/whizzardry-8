#version 450
// Alpha-tested cutouts (foliage, fences) cast their texture's silhouette.

layout(std140, set = 3, binding = 0) uniform Batch {
    vec4 channels;
    vec4 modes;
    vec4 alpha_test;
} batch;
layout(set = 3, binding = 1) uniform sampler2D material_texture;

layout(location = 0) in vec2 v_st0;

void main()
{
    if (batch.alpha_test.x != 0.0 && batch.modes.x != 0.0 &&
        texture(material_texture, v_st0).a < 0.5)
        discard;
}
