#version 450
#extension GL_GOOGLE_include_directive : require

#include "world_blocks.glsl"

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 model_view;
} pc;
#include "world_host.glsl"

#include "wizardry_vertex.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_texcoords;
layout(location = 3) in vec4 a_color;
layout(location = 4) in vec4 a_diffuse_multiplier;
layout(location = 5) in vec4 a_specular_multiplier;
layout(location = 6) in vec4 a_metadata;
layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) out vec2 v_st0;
layout(location = 3) out vec3 v_eye;
layout(location = 4) out vec2 v_st1;
#ifdef WZ_PER_PIXEL
layout(location = 5) out vec3 v_normal;
layout(location = 6) flat out vec4 v_object; // eye-space bounds centre, radius
layout(location = 7) flat out float v_material;
layout(location = 8) out float v_alpha;
layout(location = 9) out vec4 v_color;
layout(location = 10) out vec4 v_diffuse_multiplier;
layout(location = 11) out vec4 v_specular_multiplier;
#endif

void main()
{
    mat3 basis = mat3(pc.model_view);
    float scale = max(length(basis[0]), max(length(basis[1]), length(basis[2])));
    for (int axis = 0; axis < 3; ++axis) {
        float length = length(basis[axis]);
        if (length != 0.0) basis[axis] /= length;
    }
    mat3 normal_matrix = transpose(inverse(basis));
    if (instance.masks.y != 0u) normal_matrix = -normal_matrix;
    eye_center = (pc.model_view * vec4(instance.bounds.xyz, 1.0)).xyz;
    eye_radius = instance.bounds.w * scale;
    WzVertex shaded = wzShadeVertex(pc.model_view, normal_matrix, a_position, a_normal,
                                     a_metadata, a_texcoords, a_color,
                                     a_diffuse_multiplier, a_specular_multiplier);
    gl_Position = pc.projection * shaded.eye;
    v_diffuse = shaded.diffuse;
    v_specular = shaded.specular;
    v_st0 = shaded.st0;
    v_eye = shaded.eye.xyz;
    v_st1 = shaded.st1;
#ifdef WZ_PER_PIXEL
    v_normal = shaded.normal;
    v_object = vec4(eye_center, eye_radius);
    v_material = a_metadata.x;
    v_alpha = a_metadata.z;
    v_color = a_color;
    v_diffuse_multiplier = a_diffuse_multiplier;
    v_specular_multiplier = a_specular_multiplier;
#endif
}
