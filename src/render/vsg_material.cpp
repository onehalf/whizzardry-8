#include "wiz8/render/vsg_material.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_float3x3.hpp>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_double2.hpp>
#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/ext/vector_uint4.hpp>
#include <glm/matrix.hpp>
#include <vsg/core/Array.h>
#include <vsg/core/Array2D.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec3.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderModule.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>
#include <vulkan/vulkan_core.h>

#include "shadow_frag.spv.h"
#include "shadow_vert.spv.h"
#include "wiz8/math.h"
#include "wiz8/render/draw_inputs.h"
#include "wiz8/render/draw_shader.h"
#include "wiz8/render/fog.h"
#include "wiz8/render/graphics_settings.h"
#include "wiz8/render/light.h"
#include "wiz8/render/material.h"
#include "wiz8/render/mesh.h"
#include "wiz8/render/mesh_instance.h"
#include "wiz8/render/texture.h"
#include "wiz8/render/vertex_channels.h"
#include "world_frag.spv.h"
#include "world_ppl_frag.spv.h"
#include "world_ppl_vert.spv.h"
#include "world_vert.spv.h"

namespace render {
namespace {
constexpr std::size_t light_vectors = 8;
constexpr std::size_t fog_vectors = 2;
constexpr std::size_t material_vectors = 6;
// View header: depth cue, fog colour, counts, then the sun shadow block.
constexpr std::size_t shadow_base = 3;
constexpr std::size_t shadow_vectors = 3 + ShadowCascades::max_cascades * 4;
// Torch shadows: header, eye-to-world rotation, then per light its position,
// colour and six face matrices.
constexpr std::size_t light_shadow_base = shadow_base + shadow_vectors;
constexpr std::size_t light_shadow_vectors_each = 2 + LightShadows::faces * 4;
constexpr std::size_t light_shadow_vectors =
    4 + LightShadows::max_lights * light_shadow_vectors_each;
constexpr std::size_t light_base = light_shadow_base + light_shadow_vectors;
// world_blocks.glsl repeats these.
static_assert(light_shadow_base == 22 && light_shadow_vectors_each == 26 && light_base == 130);

std::uint32_t disabledChannels(std::uint32_t shader)
{
    using enum VertexChannels::e_channel;
    std::uint32_t disabled = 0;
    if (!(shader & DrawShader::MASK_FOG)) disabled |= 1u << CHANNEL_FOG;
    if (!(shader & DrawShader::MASK_GRADIENT)) {
        disabled |= (1u << CHANNEL_DIFFUSE) | (1u << CHANNEL_ALPHA);
    } else if (!(shader & DrawShader::MASK_ALPHATEST) &&
               ((shader >> DrawShader::DSTBLEND_SHIFT) & 7) != DrawShader::DSTBLEND_SRC_ALPHA &&
               ((shader >> DrawShader::DSTBLEND_SHIFT) & 7) !=
                   DrawShader::DSTBLEND_ONE_MINUS_SRC_ALPHA &&
               ((shader >> DrawShader::SRCBLEND_SHIFT) & 3) != DrawShader::SRCBLEND_SRC_ALPHA &&
               ((shader >> DrawShader::SRCBLEND_SHIFT) & 3) !=
                   DrawShader::SRCBLEND_ONE_MINUS_SRC_ALPHA) {
        disabled |= 1u << CHANNEL_ALPHA;
    }
    if (!(shader & DrawShader::MASK_SECONDARY_GRADIENT)) disabled |= 1u << CHANNEL_SPECULAR;
    if (!(shader & DrawShader::MASK_TEXTURING)) {
        disabled |=
            (1u << CHANNEL_ST0) | (1u << CHANNEL_ST1) | (1u << CHANNEL_Q0) | (1u << CHANNEL_Q1);
    } else if (!(shader & (DrawShader::MASK_DETAILCOLOR0 | DrawShader::MASK_DETAILALPHA0 |
                           DrawShader::MASK_DETAILCOLOR1 | DrawShader::MASK_DETAILALPHA1))) {
        disabled |= (1u << CHANNEL_ST1) | (1u << CHANNEL_Q1);
    }
    return disabled;
}

void packMaterial(Material& material, double time, std::span<glm::vec4> out)
{
    const auto info = material.getMaterialInfo();
    const auto scroll = glm::vec2(glm::fract(glm::dvec2(material.uv_velocity) * time));
    out[0] = info.diffuse;
    out[1] = info.ambient;
    out[2] = info.specular;
    out[3] = info.emissive;
    out[4] = {info.shininess, info.fog_scale, static_cast<float>(material.operations),
              static_cast<float>(info.disabled_channels)};
    out[5] = {static_cast<float>(material.uv_mapping), scroll.x, scroll.y, 0};
}
} // namespace

RenderContext::RenderContext(VkExtent2D extent, int samples)
    : viewport(vsg::ViewportState::create(extent))
{
    using Bindings = vsg::DescriptorSetLayoutBindings;
    constexpr auto vertex = VK_SHADER_STAGE_VERTEX_BIT;
    constexpr auto fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr auto uniform = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constexpr auto storage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    view_layout = vsg::DescriptorSetLayout::create(
        Bindings{{0, storage, 1, vertex | fragment, nullptr},
                 {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
                 {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr}});
    // Per-pixel lighting evaluates the material and light rules in the fragment stage.
    instance_layout =
        vsg::DescriptorSetLayout::create(Bindings{{0, uniform, 1, vertex | fragment, nullptr}});
    pass_layout =
        vsg::DescriptorSetLayout::create(Bindings{{0, storage, 1, vertex | fragment, nullptr}});
    batch_layout = vsg::DescriptorSetLayout::create(
        Bindings{{0, uniform, 1, vertex | fragment, nullptr},
                 {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr},
                 {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, fragment, nullptr}});
    layout = vsg::PipelineLayout::create(
        vsg::DescriptorSetLayouts{view_layout, instance_layout, pass_layout, batch_layout},
        vsg::PushConstantRanges{{vertex, 0, 128}});
    const auto module = [](std::span<const std::uint32_t> code) {
        return vsg::ShaderModule::create(vsg::ShaderModule::SPIRV(code.begin(), code.end()));
    };
    const auto& graphics = CurrentGraphicsSettings();
    if (graphics.per_pixel_lighting)
        stages = {vsg::ShaderStage::create(vertex, "main", module(world_ppl_vert)),
                  vsg::ShaderStage::create(fragment, "main", module(world_ppl_frag))};
    else
        stages = {vsg::ShaderStage::create(vertex, "main", module(world_vert)),
                  vsg::ShaderStage::create(fragment, "main", module(world_frag))};
    stages[1]->specializationConstants[0] =
        vsg::uintValue::create(graphics.alpha_to_coverage && samples > 1 ? 1u : 0u);
    stages[1]->specializationConstants[1] = vsg::intValue::create(samples);
    const auto light_max = vsg::floatValue::create(graphics.hdr ? graphics.light_max : 1.0f);
    stages[0]->specializationConstants[2] = light_max;
    stages[1]->specializationConstants[2] = light_max;
    shadow_stages = {vsg::ShaderStage::create(vertex, "main", module(shadow_vert)),
                     vsg::ShaderStage::create(fragment, "main", module(shadow_frag))};
    auto pixel = vsg::ubvec4Array2D::create(1, 1, vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
    pixel->at(0, 0) = vsg::ubvec4(255, 255, 255, 255);
    white = vsg::ImageInfo::create(vsg::Sampler::create(), pixel);
    // Depth 1 is the far plane of every cascade and cube face: nothing is in shadow.
    auto far = vsg::floatArray2D::create(1, 1, vsg::Data::Properties{VK_FORMAT_R32_SFLOAT});
    far->at(0, 0) = 1.0f;
    auto nearest = vsg::Sampler::create();
    nearest->magFilter = nearest->minFilter = VK_FILTER_NEAREST;
    shadow_map = vsg::ImageInfo::create(nearest, far);
    light_shadow_map = shadow_map;
}

vsg::ref_ptr<vsg::BindGraphicsPipeline>
RenderContext::shadowPipeline(const vsg::BindGraphicsPipeline* scene) const
{
    const auto found = shadow_pipelines.find(scene);
    return found == shadow_pipelines.end() ? vsg::ref_ptr<vsg::BindGraphicsPipeline>{}
                                           : found->second;
}

vsg::ref_ptr<vsg::BindGraphicsPipeline> RenderContext::pipeline(std::uint32_t shader, FaceCull cull,
                                                                Winding winding,
                                                                std::int32_t polygon_offset,
                                                                Target target)
{
    auto& found = pipelines[{shader, cull, winding, polygon_offset, target}];
    if (found) return found;
    static constexpr std::array source{VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
                                       VK_BLEND_FACTOR_SRC_ALPHA,
                                       VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    static constexpr std::array destination{
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_ONE};
    vsg::VertexInputState::Bindings bindings;
    vsg::VertexInputState::Attributes attributes;
    for (std::uint32_t location = 0; location < 7; ++location) {
        const bool geometry = location < 2;
        bindings.push_back(
            {location, static_cast<std::uint32_t>(geometry ? sizeof(vsg::vec3) : sizeof(vsg::vec4)),
             VK_VERTEX_INPUT_RATE_VERTEX});
        attributes.push_back({location, location,
                              geometry ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT,
                              0});
    }
    auto rasterization = vsg::RasterizationState::create();
    rasterization->cullMode = cull == FaceCull::None   ? VK_CULL_MODE_NONE
                              : cull == FaceCull::Back ? VK_CULL_MODE_BACK_BIT
                                                       : VK_CULL_MODE_FRONT_BIT;
    rasterization->depthBiasEnable = polygon_offset != 0;
    // VSG uses reverse Z: increasing depth moves an overlay toward the eye.
    rasterization->depthBiasConstantFactor = static_cast<float>(polygon_offset);
    rasterization->depthBiasSlopeFactor = static_cast<float>(polygon_offset);
    rasterization->frontFace =
        winding == Winding::Standard ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineColorBlendAttachmentState blend{};
    const auto source_index = (shader >> DrawShader::SRCBLEND_SHIFT) & 3u;
    const auto destination_index = (shader >> DrawShader::DSTBLEND_SHIFT) & 7u;
    blend.blendEnable =
        source_index != DrawShader::SRCBLEND_ONE || destination_index != DrawShader::DSTBLEND_ZERO;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = source[source_index];
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = destination[destination_index];
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = shader & DrawShader::MASK_COLOR_WRITE
                               ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
                               : 0;
    auto depth = vsg::DepthStencilState::create();
    depth->depthWriteEnable = (shader & DrawShader::MASK_DEPTH_WRITE) != 0;
    // Retail material comparisons describe forward Z; every VSG view uses
    // reverse Z so ordinary VSG subgraphs share the same depth attachment.
    static constexpr std::array compare{VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_GREATER,
                                        VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_GREATER_OR_EQUAL,
                                        VK_COMPARE_OP_LESS,          VK_COMPARE_OP_NOT_EQUAL,
                                        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    depth->depthCompareOp = compare[shader & DrawShader::PASS_MASK];
    depth->depthTestEnable =
        depth->depthCompareOp != VK_COMPARE_OP_ALWAYS || depth->depthWriteEnable;
    vsg::GraphicsPipelineStates states{
        vsg::VertexInputState::create(bindings, attributes),
        vsg::InputAssemblyState::create(),
        rasterization,
        vsg::MultisampleState::create(),
        vsg::ColorBlendState::create(vsg::ColorBlendState::ColorBlendAttachments{blend}),
        depth,
        viewport};
    found =
        vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(layout, stages, states));
    // Opaque and alpha-tested geometry that writes depth casts sun shadows.
    // The shadow pass draws both faces (single-sided walls still block the
    // sun) and pushes depth away from the light against acne.
    const bool opaque = !blend.blendEnable || (shader & DrawShader::MASK_ALPHATEST);
    if (target == Target::Scene && opaque && depth->depthWriteEnable && blend.colorWriteMask != 0 &&
        polygon_offset == 0) {
        auto shadow_raster = vsg::RasterizationState::create();
        shadow_raster->cullMode = VK_CULL_MODE_NONE;
        shadow_raster->depthBiasEnable = VK_TRUE;
        shadow_raster->depthBiasConstantFactor = 2.0f;
        shadow_raster->depthBiasSlopeFactor = 2.5f;
        auto shadow_depth = vsg::DepthStencilState::create();
        shadow_depth->depthTestEnable = shadow_depth->depthWriteEnable = VK_TRUE;
        shadow_depth->depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        vsg::GraphicsPipelineStates shadow_states{
            vsg::VertexInputState::create(bindings, attributes),
            vsg::InputAssemblyState::create(),
            shadow_raster,
            vsg::MultisampleState::create(),
            vsg::ColorBlendState::create(vsg::ColorBlendState::ColorBlendAttachments{}),
            shadow_depth,
            viewport};
        shadow_pipelines[found.get()] = vsg::BindGraphicsPipeline::create(
            vsg::GraphicsPipeline::create(layout, shadow_stages, shadow_states));
    }
    return found;
}

vsg::ref_ptr<vsg::ImageInfo> RenderContext::texture(Texture* source, int reduction)
{
    return source ? source->imageInfo(reduction) : white;
}

vsg::ref_ptr<vsg::BindDescriptorSet> RenderContext::descriptor(std::uint32_t set,
                                                               vsg::ref_ptr<vsg::Data> data)
{
    const auto descriptor_layout = set == 0   ? view_layout
                                   : set == 1 ? instance_layout
                                              : pass_layout;
    const auto type =
        set == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    vsg::Descriptors descriptors{vsg::DescriptorBuffer::create(data, 0, 0, type)};
    if (set == 0) {
        descriptors.push_back(vsg::DescriptorImage::create(shadow_map, 1));
        descriptors.push_back(vsg::DescriptorImage::create(light_shadow_map, 2));
    }
    return vsg::BindDescriptorSet::create(
        VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set,
        vsg::DescriptorSet::create(descriptor_layout, std::move(descriptors)));
}

vsg::ref_ptr<vsg::BindDescriptorSet>
RenderContext::batchSet(std::uint32_t shader, vsg::ref_ptr<vsg::ImageInfo> image,
                        vsg::ref_ptr<vsg::ImageInfo> detail_image)
{
    const std::array values{
        glm::vec4(static_cast<float>(~disabledChannels(shader) & 0xffffu), 0, 0, 0),
        glm::vec4(
            (shader & DrawShader::MASK_TEXTURING) ? 1.f : 0.f,
            static_cast<float>((shader & DrawShader::MASK_GRADIENT) >> DrawShader::GRADIENT_SHIFT),
            (shader & DrawShader::MASK_SECONDARY_GRADIENT) ? 1.f : 0.f,
            static_cast<float>((shader & DrawShader::MASK_FOG) >> DrawShader::FOG_SHIFT)),
        glm::vec4((shader & DrawShader::MASK_ALPHATEST) ? 1.f : 0.f, 0, 0, 0)};
    return vsg::BindDescriptorSet::create(
        VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3,
        vsg::DescriptorSet::create(
            batch_layout,
            vsg::Descriptors{
                vsg::DescriptorBuffer::create(makeArray(values, vsg::STATIC_DATA), 0),
                vsg::DescriptorImage::create(image ? image : white, 1),
                vsg::DescriptorImage::create(detail_image ? detail_image : white, 2)}));
}

std::vector<glm::vec4> packView(const ScenePass& pass, std::span<const glm::vec4> eye_clip_planes,
                                std::span<const PreparedLight> lights)
{
    const auto fog_base = light_base + lights.size() * light_vectors;
    const auto clip_base = fog_base + pass.fogs.size() * fog_vectors;
    std::vector<glm::vec4> result(clip_base + eye_clip_planes.size());
    result[0] = {pass.depth_cue.minimum, pass.depth_cue.maximum, pass.depth_cue.near_scale,
                 pass.depth_cue.far_scale};
    result[1] = pass.fog_color;
    result[2] = {static_cast<float>(lights.size()), static_cast<float>(pass.fogs.size()),
                 static_cast<float>(eye_clip_planes.size()), static_cast<float>(fog_base)};
    for (std::size_t light = 0; light < lights.size(); ++light)
        std::ranges::copy(lights[light], result.begin() + light_base + light * light_vectors);
    if (const auto* shadow = pass.shadow; shadow && shadow->count > 0) {
        result[shadow_base] = {static_cast<float>(shadow->count), shadow->strength,
                               shadow->atlas_texel, shadow->fade_end};
        result[shadow_base + 1] = shadow->texel_world;
        result[shadow_base + 2] = glm::vec4(shadow->sun_eye, shadow->fade_start);
        for (int cascade = 0; cascade < shadow->count; ++cascade)
            for (int column = 0; column < 4; ++column)
                result[shadow_base + 3 + cascade * 4 + column] =
                    shadow->eye_to_atlas[cascade][column];
    }
    if (const auto* shadow = pass.light_shadow; shadow && shadow->count > 0) {
        result[light_shadow_base] = {static_cast<float>(shadow->count), shadow->strength,
                                     shadow->atlas_texel.x, shadow->atlas_texel.y};
        for (int column = 0; column < 3; ++column)
            result[light_shadow_base + 1 + column] = glm::vec4(shadow->eye_to_world[column], 0);
        for (int light = 0; light < shadow->count; ++light) {
            const auto& source = shadow->lights[light];
            const auto base = light_shadow_base + 4 + light * light_shadow_vectors_each;
            result[base] = glm::vec4(source.eye_position, source.range);
            result[base + 1] = glm::vec4(source.color, source.texel_tangent);
            for (int face = 0; face < LightShadows::faces; ++face)
                for (int column = 0; column < 4; ++column)
                    result[base + 2 + face * 4 + column] = source.eye_to_atlas[face][column];
        }
    }
    for (std::size_t layer = 0; layer < pass.fogs.size(); ++layer) {
        const auto& fog = *pass.fogs[layer];
        const float normalization = fog.fog_end == fog.fog_start
                                        ? 1e8f
                                        : static_cast<float>(1.0 / (fog.fog_end - fog.fog_start));
        result[fog_base + layer * fog_vectors] = {static_cast<float>(fog.fog_start), normalization,
                                                  fog.density, static_cast<float>(fog.fog_end)};
        result[fog_base + layer * fog_vectors + 1] = {std::bit_cast<float>(fog.group_mask), 0, 0,
                                                      0};
    }
    std::ranges::copy(eye_clip_planes, result.begin() + static_cast<std::ptrdiff_t>(clip_base));
    return result;
}

std::vector<glm::vec4> packPass(std::span<const std::shared_ptr<Material>> materials,
                                std::uint32_t draw_flags, bool has_normals, double time)
{
    // A default slot also supports effects with no authored material table.
    const auto count = std::max(std::size_t{1}, materials.size());
    std::vector<glm::vec4> result(1 + count * material_vectors);
    result[0] = {static_cast<float>(draw_flags), has_normals ? 1.f : 0.f, 0, 0};
    Material fallback;
    for (std::size_t slot = 0; slot < count; ++slot) {
        auto* material =
            slot < materials.size() && materials[slot] ? materials[slot].get() : &fallback;
        packMaterial(*material, time,
                     std::span(result).subspan(1 + slot * material_vectors, material_vectors));
    }
    return result;
}

InstanceBlock packInstance(const MeshInstance& instance, const Mesh& mesh, const ScenePass& pass,
                           bool reverse_normals)
{
    const auto ambient = mesh.vertex_lighting_ready
                             ? glm::vec4(0)
                             : glm::vec4((glm::vec3(pass.ambient_light) * instance.light_scale +
                                          pass.frame.environment_offset) *
                                             pass.frame.brightness,
                                         1);
    return {.ambient = ambient,
            .bounds = glm::vec4(mesh.bounds_center, mesh.bounds_radius),
            .overrides = {instance.diffuse_scale_enabled ? 1.f : 0.f, instance.diffuse_scale,
                          instance.emissive_override_enabled ? 1.f : 0.f,
                          instance.emissive_override},
            .masks = {instance.getExclusionMask() | pass.exclusion_mask, reverse_normals ? 1u : 0u,
                      0, 0}};
}

vsg::ref_ptr<vsg::vec4Array> makeArray(std::span<const glm::vec4> values,
                                       vsg::DataVariance variance)
{
    auto result = vsg::vec4Array::create(static_cast<std::uint32_t>(values.size()));
    result->properties.dataVariance = variance;
    store(*result, values);
    return result;
}

void store(vsg::vec4Array& destination, std::span<const glm::vec4> values)
{
    if (values.size() > destination.size())
        throw std::length_error("Render state exceeds compiled buffer");
    bool changed = false;
    for (std::size_t index = 0; index < destination.size(); ++index) {
        // View light collection may use fewer records than its compiled capacity.
        const auto value = index < values.size() ? values[index] : glm::vec4(0);
        auto& current = destination.at(index);
        for (int component = 0; component < 4; ++component) {
            // Group masks are serialized through float bits, including NaN bit
            // patterns. Compare those bits so unchanged masks stay unchanged.
            if (std::bit_cast<std::uint32_t>(current[component]) ==
                std::bit_cast<std::uint32_t>(value[component]))
                continue;
            current = vsg::vec4(value.x, value.y, value.z, value.w);
            changed = true;
            break;
        }
    }
    if (changed) destination.dirty();
}

vsg::ref_ptr<vsg::vec4Array> makeInstanceArray(const InstanceBlock& block)
{
    auto result = vsg::vec4Array::create(4);
    result->properties.dataVariance = vsg::DYNAMIC_DATA;
    store(*result, block);
    return result;
}

void store(vsg::vec4Array& destination, const InstanceBlock& block)
{
    const std::array records{
        block.ambient, block.bounds, block.overrides,
        glm::vec4(std::bit_cast<float>(block.masks.x), std::bit_cast<float>(block.masks.y),
                  std::bit_cast<float>(block.masks.z), std::bit_cast<float>(block.masks.w))};
    store(destination, records);
}

std::uint32_t fadingShader(std::uint32_t shader)
{
    return (shader & 0xffffd7bfu) | 0x44a0u;
}

PreparedLight prepareLightAt(const Light& light, const glm::mat4& model_view, float brightness)
{
    PreparedLight result{};
    // Low bits match the retail light operations used by the shader.
    std::uint32_t flags = 1;
    if (light.enable_flags & (1u << Light::ENABLE_RANGE_NEAR)) flags |= 1u << 6;
    if (light.enable_flags & (1u << Light::ENABLE_RANGE_FAR)) flags |= 1u << 7;
    flags |= static_cast<std::uint32_t>(light.attenuation_model) << 8;
    const std::array colors{light.ambient, light.diffuse, light.specular};
    bool active = false;
    for (std::size_t channel = 0; channel < colors.size(); ++channel) {
        if (colors[channel] == glm::vec3(0)) continue;
        active = true;
        flags |= 1u << (10 + channel);
        result[1 + channel] = glm::vec4(colors[channel] * light.intensity * brightness, 0);
    }
    if (!active) flags &= ~1u;
    glm::vec3 location(model_view[3]);
    if (light.enable_flags & (1u << Light::ENABLE_DIRECTIONAL)) {
        flags |= 4;
        location = normalizedOrZero(-glm::vec3(model_view[2]));
    } else {
        if ((light.enable_flags & (1u << Light::ENABLE_SPOT)) && light.spot_angle > 0 &&
            light.spot_exponent != 0) {
            flags |= 2;
            result[4] =
                glm::vec4(normalizedOrZero(glm::transpose(glm::mat3(glm::inverse(model_view))) *
                                           light.spot_direction),
                          std::cos(light.spot_angle));
        }
        if (light.attenuation_model == Light::ATTENUATION_OPENGL) {
            flags |= 8;
            if (std::abs(light.opengl_attenuation.y) < 5.9604645e-08f &&
                std::abs(light.opengl_attenuation.z) < 5.9604645e-08f) {
                if (light.opengl_attenuation.x == 1 ||
                    std::abs(light.opengl_attenuation.x) < 5.9604645e-08f)
                    flags &= ~8u;
                flags |= 16;
            }
        } else if (light.attenuation_model == Light::ATTENUATION_3DSTUDIO_MAX) {
            const float scale = DrawTransform{.model_view = model_view}.maxModelViewScale();
            result[5] = {float(scale * light.near_start),
                         light.near_start == light.near_end
                             ? 1.f
                             : float(1 / (scale * (light.near_end - light.near_start))),
                         float(scale * light.far_end),
                         light.far_start == light.far_end
                             ? 1.f
                             : float(1 / (scale * (light.far_end - light.far_start)))};
            if (light.enable_flags & (1u << Light::ENABLE_RANGE_FAR)) flags |= 32;
        }
    }
    result[0] = glm::vec4(location, float(flags));
    result[6] = glm::vec4(light.opengl_attenuation, light.spot_exponent);
    result[7] = {std::bit_cast<float>(light.group_mask), 0, 0, 0};
    return result;
}

} // namespace render
