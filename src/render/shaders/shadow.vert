#version 450
// Depth-only sun shadow pass. VSG pushes the cascade's projection and the
// light-space model view into the first 128 push-constant bytes.

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 model_view;
} pc;

layout(location = 0) in vec3 a_position;
layout(location = 2) in vec4 a_texcoords;
layout(location = 0) out vec2 v_st0;

void main()
{
    gl_Position = pc.projection * (pc.model_view * vec4(a_position, 1.0));
    v_st0 = a_texcoords.xy;
}
