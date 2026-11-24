#include "wiz8/render/post_process.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

#include <glm/mat4x4.hpp>
#include <vsg/all.h>

#include "post_bloom_down_comp.spv.h"
#include "post_bloom_up_comp.spv.h"
#include "post_blur_comp.spv.h"
#include "post_composite_frag.spv.h"
#include "post_composite_vert.spv.h"
#include "post_depth_comp.spv.h"
#include "post_depth_ms_comp.spv.h"
#include "post_ssao_comp.spv.h"
#include "wiz8/render/graphics_settings.h"
#include "wiz8/render/quality.h"

namespace render {
namespace {
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kAoFormat = VK_FORMAT_R16_SFLOAT;
constexpr int kMaxBloomLevels = 7;

// The SSAO/blur push block: projection, world rect, pass parameters.
struct PostParameters {
    glm::mat4 projection;
    std::array<std::int32_t, 4> rect;
    glm::vec4 parameters;
};
static_assert(sizeof(PostParameters) == 96);

struct CompositeParameters {
    glm::vec4 grade;
    glm::vec4 look;
    glm::vec4 tone;
};

vsg::ref_ptr<vsg::ShaderModule> module(std::span<const std::uint32_t> code)
{
    return vsg::ShaderModule::create(vsg::ShaderModule::SPIRV(code.begin(), code.end()));
}

template<typename T> vsg::ref_ptr<vsg::ubyteArray> pushData(const T& value)
{
    auto data = vsg::ubyteArray::create(sizeof(T));
    std::memcpy(data->dataPointer(), &value, sizeof(T));
    return data;
}

template<typename T> void writePushData(vsg::ubyteArray& data, const T& value)
{
    std::memcpy(data.dataPointer(), &value, sizeof(T));
}

vsg::ref_ptr<vsg::Image> makeImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                                   std::uint32_t mip_levels = 1)
{
    auto image = vsg::Image::create();
    image->imageType = VK_IMAGE_TYPE_2D;
    image->format = format;
    image->extent = {std::max(extent.width, 1u), std::max(extent.height, 1u), 1};
    image->mipLevels = mip_levels;
    image->arrayLayers = 1;
    image->samples = samples;
    image->usage = usage;
    return image;
}

// A view of one mip level of an already allocated image.
vsg::ref_ptr<vsg::ImageView> levelView(vsg::Device* device, vsg::ref_ptr<vsg::Image> image,
                                       std::uint32_t level)
{
    auto view = vsg::ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT);
    view->subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    view->compile(device);
    return view;
}

vsg::ref_ptr<vsg::Sampler> makeSampler(VkFilter filter)
{
    auto sampler = vsg::Sampler::create();
    sampler->magFilter = sampler->minFilter = filter;
    sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler->addressModeU = sampler->addressModeV = sampler->addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler->maxLod = 0.0f;
    return sampler;
}

vsg::ref_ptr<vsg::ImageMemoryBarrier> toGeneral(vsg::ref_ptr<vsg::Image> image)
{
    return vsg::ImageMemoryBarrier::create(
        0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, image->mipLevels, 0, 1});
}

vsg::ref_ptr<vsg::PipelineBarrier> computeToCompute()
{
    return vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        vsg::MemoryBarrier::create(VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT));
}

vsg::ref_ptr<vsg::RenderPass> scenePass(vsg::Device* device, VkSampleCountFlagBits samples,
                                        VkFormat depth_format)
{
    const bool multisampled = samples != VK_SAMPLE_COUNT_1_BIT;
    vsg::AttachmentDescription color{};
    color.format = kHdrFormat;
    color.samples = samples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vsg::AttachmentDescription resolve = color;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vsg::AttachmentDescription depth{};
    depth.format = depth_format;
    depth.samples = samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    vsg::RenderPass::Attachments attachments{color};
    if (multisampled) attachments.push_back(resolve);
    attachments.push_back(depth);
    const auto depth_index = static_cast<std::uint32_t>(attachments.size() - 1);

    vsg::SubpassDescription subpass;
    subpass.colorAttachments.push_back({0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    if (multisampled)
        subpass.resolveAttachments.push_back({1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    subpass.depthStencilAttachments.push_back(
        {depth_index, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});

    constexpr auto attachment_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr auto reader_stages =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vsg::SubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = attachment_stages | reader_stages;
    before.dstStageMask = attachment_stages;
    before.srcAccessMask = 0;
    before.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    before.dependencyFlags = 0;
    vsg::SubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = attachment_stages;
    after.dstStageMask = reader_stages;
    after.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    after.dependencyFlags = 0;
    return vsg::RenderPass::create(device, attachments, vsg::RenderPass::Subpasses{subpass},
                                   vsg::RenderPass::Dependencies{before, after});
}

// VSG's setClearValues recognizes depth only by an attachment-optimal final
// layout; the scene's depth ends read-only for sampling. Reverse Z clears to 0.
void setClearValues(vsg::RenderGraph& graph, const glm::vec4& clear)
{
    const auto& attachments = graph.getRenderPass()->attachments;
    graph.clearValues.resize(attachments.size());
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        if (attachments[i].format == kHdrFormat)
            graph.clearValues[i].color = {{clear.r, clear.g, clear.b, clear.a}};
        else
            graph.clearValues[i].depthStencil = {0.0f, 0};
    }
}

// One compute pass: pipeline, descriptor set, push constants and dispatch.
struct ComputePass {
    vsg::ref_ptr<vsg::Commands> commands = vsg::Commands::create();
    vsg::ref_ptr<vsg::ubyteArray> push;
    vsg::ref_ptr<vsg::Dispatch> dispatch = vsg::Dispatch::create(1, 1, 1);

    ComputePass(vsg::ref_ptr<vsg::BindComputePipeline> pipeline,
                vsg::ref_ptr<vsg::PipelineLayout> layout,
                vsg::ref_ptr<vsg::DescriptorSetLayout> set_layout, vsg::Descriptors descriptors,
                vsg::ref_ptr<vsg::ubyteArray> push_data)
        : push(std::move(push_data))
    {
        commands->addChild(pipeline);
        commands->addChild(vsg::BindDescriptorSet::create(
            VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
            vsg::DescriptorSet::create(set_layout, std::move(descriptors))));
        commands->addChild(vsg::PushConstants::create(VK_SHADER_STAGE_COMPUTE_BIT, 0, push));
        commands->addChild(dispatch);
    }
    void size(std::uint32_t width, std::uint32_t height)
    {
        dispatch->groupCountX = (width + 7) / 8;
        dispatch->groupCountY = (height + 7) / 8;
    }
};
} // namespace

struct PostProcess::Targets {
    VkExtent2D extent{};
    vsg::ref_ptr<vsg::RenderGraph> scene;
    vsg::ref_ptr<vsg::Image> color, depth, single_depth, raw_ao, ao, bloom;
    std::vector<VkExtent2D> bloom_sizes;
    vsg::ref_ptr<vsg::PipelineBarrier> prepare;
    std::unique_ptr<ComputePass> depth_copy, ssao, blur;
    std::vector<std::unique_ptr<ComputePass>> bloom_down, bloom_up;
    vsg::ref_ptr<vsg::Commands> compute = vsg::Commands::create();
    vsg::ref_ptr<vsg::View> composite;
    vsg::ref_ptr<vsg::ubyteArray> composite_push;
};

namespace {
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

// Depth only; the atlas ends ready for sampling by the world pass.
vsg::ref_ptr<vsg::RenderPass> shadowPass(vsg::Device* device)
{
    vsg::AttachmentDescription depth{};
    depth.format = kShadowFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    vsg::SubpassDescription subpass;
    subpass.depthStencilAttachments.push_back(
        {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
    constexpr auto tests =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    vsg::SubpassDependency before{};
    before.srcSubpass = VK_SUBPASS_EXTERNAL;
    before.dstSubpass = 0;
    before.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | tests;
    before.dstStageMask = tests;
    before.srcAccessMask = 0;
    before.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vsg::SubpassDependency after{};
    after.srcSubpass = 0;
    after.dstSubpass = VK_SUBPASS_EXTERNAL;
    after.srcStageMask = tests;
    after.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    after.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    return vsg::RenderPass::create(device, vsg::RenderPass::Attachments{depth},
                                   vsg::RenderPass::Subpasses{subpass},
                                   vsg::RenderPass::Dependencies{before, after});
}
} // namespace

PostProcess::PostProcess(vsg::ref_ptr<vsg::Device> device, VkSampleCountFlagBits samples,
                         VkFormat depth_format, const GraphicsSettings& settings)
    : device_(std::move(device)), samples_(samples), depth_format_(depth_format),
      settings_(settings)
{
    const bool sun = SunShadowsEnabled(settings_), light = LightShadowsEnabled(settings_);
    if (!sun && !light) return;
    const auto pass = shadowPass(device_.get());
    const auto make = [&](ShadowAtlas kind, std::uint32_t width, std::uint32_t height) {
        auto atlas =
            makeImage(kShadowFormat, {width, height},
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        auto view = vsg::createImageView(device_.get(), atlas, VK_IMAGE_ASPECT_DEPTH_BIT);
        auto& target = shadows_[static_cast<std::size_t>(kind)];
        target.graph = vsg::RenderGraph::create();
        target.graph->framebuffer =
            vsg::Framebuffer::create(pass, vsg::ImageViews{view}, width, height, 1);
        target.graph->renderArea = {{0, 0}, {width, height}};
        target.graph->viewportState = vsg::ViewportState::create(VkExtent2D{width, height});
        target.graph->windowResizeHandler = {};
        // Set by hand: VSG takes an attachment for depth only by an attachment-optimal final
        // layout.
        target.graph->clearValues.resize(1);
        target.graph->clearValues[0].depthStencil = {1.0f, 0};
        target.info = vsg::ImageInfo::create(makeSampler(VK_FILTER_NEAREST), view,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    };
    if (sun) {
        const auto size = static_cast<std::uint32_t>(std::max(settings_.shadow_size, 64));
        make(ShadowAtlas::Sun, size, size);
    }
    if (light) {
        // One row of six cube faces per shadowed light.
        const auto face = static_cast<std::uint32_t>(std::max(settings_.light_shadow_size, 16));
        make(ShadowAtlas::Light, face * 6,
             face * static_cast<std::uint32_t>(settings_.shadow_lights));
    }
}

void PostProcess::frameSubmitted()
{
    // That frame cleared the atlas into its sampled layout.
    shadow_ready_ = shadow_ready_ || shadow_recorded_;
}

void PostProcess::addShadowView(ShadowAtlas atlas, vsg::ref_ptr<vsg::View> view)
{
    auto& target = shadows_[static_cast<std::size_t>(atlas)];
    if (!target.graph) return;
    target.views.push_back(std::move(view));
    buildFrameCommands();
}

PostProcess::~PostProcess() = default;

void PostProcess::resize(VkExtent2D extent)
{
    if (targets_ && targets_->extent.width == extent.width &&
        targets_->extent.height == extent.height)
        return;
    auto targets = std::make_unique<Targets>();
    auto* device = device_.get();
    targets->extent = extent;
    const bool multisampled = samples_ != VK_SAMPLE_COUNT_1_BIT;

    // Scene target.
    targets->color = makeImage(kHdrFormat, extent,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    auto color_view = vsg::createImageView(device, targets->color, VK_IMAGE_ASPECT_COLOR_BIT);
    targets->depth = makeImage(
        depth_format_, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, samples_);
    auto depth_view = vsg::createImageView(device, targets->depth, VK_IMAGE_ASPECT_DEPTH_BIT);
    vsg::ImageViews attachments{color_view, depth_view};
    if (multisampled) {
        auto ms =
            makeImage(kHdrFormat, extent,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                      samples_);
        attachments = {vsg::createImageView(device, ms, VK_IMAGE_ASPECT_COLOR_BIT), color_view,
                       depth_view};
    }
    auto pass = scenePass(device, samples_, depth_format_);
    targets->scene = vsg::RenderGraph::create();
    targets->scene->framebuffer =
        vsg::Framebuffer::create(pass, attachments, extent.width, extent.height, 1);
    targets->scene->renderArea = {{0, 0}, extent};
    targets->scene->viewportState = vsg::ViewportState::create(extent);
    targets->scene->windowResizeHandler = {};
    setClearValues(*targets->scene, {0, 0, 0, 1});

    // Post targets.
    constexpr auto storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    targets->single_depth = makeImage(VK_FORMAT_R32_SFLOAT, extent, storage);
    targets->raw_ao = makeImage(kAoFormat, extent, storage);
    targets->ao = makeImage(kAoFormat, extent, storage);
    auto single_depth_view =
        vsg::createImageView(device, targets->single_depth, VK_IMAGE_ASPECT_COLOR_BIT);
    auto raw_ao_view = vsg::createImageView(device, targets->raw_ao, VK_IMAGE_ASPECT_COLOR_BIT);
    auto ao_view = vsg::createImageView(device, targets->ao, VK_IMAGE_ASPECT_COLOR_BIT);
    const auto smallest = std::min(extent.width, extent.height);
    const int levels = std::clamp(static_cast<int>(std::bit_width(std::max(smallest, 1u))) - 4, 1,
                                  kMaxBloomLevels);
    for (int level = 0; level < levels; ++level)
        targets->bloom_sizes.push_back({std::max(1u, extent.width >> (level + 1)),
                                        std::max(1u, extent.height >> (level + 1))});
    targets->bloom = makeImage(kHdrFormat, targets->bloom_sizes.front(), storage,
                               VK_SAMPLE_COUNT_1_BIT, static_cast<std::uint32_t>(levels));
    vsg::createImageView(device, targets->bloom, VK_IMAGE_ASPECT_COLOR_BIT);
    std::vector<vsg::ref_ptr<vsg::ImageView>> bloom_levels;
    for (int level = 0; level < levels; ++level)
        bloom_levels.push_back(
            levelView(device, targets->bloom, static_cast<std::uint32_t>(level)));

    targets->prepare = vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        toGeneral(targets->single_depth), toGeneral(targets->raw_ao), toGeneral(targets->ao),
        toGeneral(targets->bloom));

    const auto nearest = makeSampler(VK_FILTER_NEAREST);
    const auto linear = makeSampler(VK_FILTER_LINEAR);
    const auto depth_info = vsg::ImageInfo::create(nearest, depth_view,
                                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    const auto color_info =
        vsg::ImageInfo::create(linear, color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const auto general = [](vsg::ref_ptr<vsg::Sampler> sampler, vsg::ref_ptr<vsg::ImageView> view) {
        return vsg::ImageInfo::create(sampler, view, VK_IMAGE_LAYOUT_GENERAL);
    };
    using Binding = VkDescriptorSetLayoutBinding;
    constexpr auto compute = VK_SHADER_STAGE_COMPUTE_BIT;
    constexpr auto sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    constexpr auto storage_image = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    // SSAO and its blur.
    {
        auto ssao_set = vsg::DescriptorSetLayout::create(
            vsg::DescriptorSetLayoutBindings{Binding{0, sampled, 1, compute, nullptr},
                                             Binding{1, storage_image, 1, compute, nullptr}});
        auto blur_set = vsg::DescriptorSetLayout::create(
            vsg::DescriptorSetLayoutBindings{Binding{0, sampled, 1, compute, nullptr},
                                             Binding{1, storage_image, 1, compute, nullptr},
                                             Binding{2, storage_image, 1, compute, nullptr}});
        const vsg::PushConstantRanges ranges{{compute, 0, sizeof(PostParameters)}};
        auto ssao_layout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{ssao_set}, ranges);
        auto blur_layout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{blur_set}, ranges);
        auto pipeline = [&](vsg::ref_ptr<vsg::PipelineLayout> layout,
                            std::span<const std::uint32_t> code) {
            return vsg::BindComputePipeline::create(vsg::ComputePipeline::create(
                layout, vsg::ShaderStage::create(compute, "main", module(code))));
        };
        targets->depth_copy = std::make_unique<ComputePass>(
            pipeline(ssao_layout, multisampled ? std::span<const std::uint32_t>(post_depth_ms_comp)
                                               : std::span<const std::uint32_t>(post_depth_comp)),
            ssao_layout, ssao_set,
            vsg::Descriptors{vsg::DescriptorImage::create(depth_info, 0, 0, sampled),
                             vsg::DescriptorImage::create(general(nearest, single_depth_view), 1, 0,
                                                          storage_image)},
            pushData(PostParameters{}));
        const auto single_depth_info = general(nearest, single_depth_view);
        targets->ssao = std::make_unique<ComputePass>(
            pipeline(ssao_layout, post_ssao_comp), ssao_layout, ssao_set,
            vsg::Descriptors{
                vsg::DescriptorImage::create(single_depth_info, 0, 0, sampled),
                vsg::DescriptorImage::create(general(nearest, raw_ao_view), 1, 0, storage_image)},
            pushData(PostParameters{}));
        targets->blur = std::make_unique<ComputePass>(
            pipeline(blur_layout, post_blur_comp), blur_layout, blur_set,
            vsg::Descriptors{
                vsg::DescriptorImage::create(single_depth_info, 0, 0, sampled),
                vsg::DescriptorImage::create(general(nearest, raw_ao_view), 1, 0, storage_image),
                vsg::DescriptorImage::create(general(nearest, ao_view), 2, 0, storage_image)},
            pushData(PostParameters{}));
    }

    // Bloom pyramid.
    {
        auto set = vsg::DescriptorSetLayout::create(
            vsg::DescriptorSetLayoutBindings{Binding{0, sampled, 1, compute, nullptr},
                                             Binding{1, storage_image, 1, compute, nullptr}});
        auto layout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{set},
                                                  vsg::PushConstantRanges{{compute, 0, 16}});
        auto down = vsg::BindComputePipeline::create(vsg::ComputePipeline::create(
            layout, vsg::ShaderStage::create(compute, "main", module(post_bloom_down_comp))));
        auto up = vsg::BindComputePipeline::create(vsg::ComputePipeline::create(
            layout, vsg::ShaderStage::create(compute, "main", module(post_bloom_up_comp))));
        for (int level = 0; level < levels; ++level) {
            const auto source = level == 0 ? color_info : general(linear, bloom_levels[level - 1]);
            // A narrow knee keeps near-white surfaces (the sky) out of the bloom.
            const glm::vec4 parameters{settings_.bloom_threshold, 0.1f * settings_.bloom_threshold,
                                       level == 0 ? 1.0f : 0.0f, 0};
            auto pass = std::make_unique<ComputePass>(
                down, layout, set,
                vsg::Descriptors{vsg::DescriptorImage::create(source, 0, 0, sampled),
                                 vsg::DescriptorImage::create(general(nearest, bloom_levels[level]),
                                                              1, 0, storage_image)},
                pushData(parameters));
            pass->size(targets->bloom_sizes[level].width, targets->bloom_sizes[level].height);
            targets->bloom_down.push_back(std::move(pass));
        }
        for (int level = levels - 2; level >= 0; --level) {
            auto pass = std::make_unique<ComputePass>(
                up, layout, set,
                vsg::Descriptors{vsg::DescriptorImage::create(
                                     general(linear, bloom_levels[level + 1]), 0, 0, sampled),
                                 vsg::DescriptorImage::create(general(nearest, bloom_levels[level]),
                                                              1, 0, storage_image)},
                pushData(glm::vec4(1.0f, 0, 0, 0)));
            pass->size(targets->bloom_sizes[level].width, targets->bloom_sizes[level].height);
            targets->bloom_up.push_back(std::move(pass));
        }
    }

    // Composite view, drawn in the window pass over the world viewport.
    {
        constexpr auto fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
        auto set = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
            Binding{0, sampled, 1, fragment, nullptr}, Binding{1, sampled, 1, fragment, nullptr},
            Binding{2, sampled, 1, fragment, nullptr}});
        auto layout = vsg::PipelineLayout::create(
            vsg::DescriptorSetLayouts{set},
            vsg::PushConstantRanges{{fragment, 0, 128 + sizeof(CompositeParameters)}});
        auto raster = vsg::RasterizationState::create();
        raster->cullMode = VK_CULL_MODE_NONE;
        auto depth = vsg::DepthStencilState::create();
        depth->depthTestEnable = depth->depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        vsg::GraphicsPipelineStates states{
            vsg::VertexInputState::create(),
            vsg::InputAssemblyState::create(),
            raster,
            vsg::MultisampleState::create(),
            depth,
            vsg::ColorBlendState::create(vsg::ColorBlendState::ColorBlendAttachments{blend}),
            vsg::ViewportState::create(extent)};
        auto pipeline = vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(
            layout,
            vsg::ShaderStages{
                vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main",
                                         module(post_composite_vert)),
                vsg::ShaderStage::create(fragment, "main", module(post_composite_frag))},
            states));
        targets->composite_push = pushData(CompositeParameters{});
        auto state = vsg::StateGroup::create();
        state->add(pipeline);
        state->add(vsg::BindDescriptorSet::create(
            VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
            vsg::DescriptorSet::create(
                set, vsg::Descriptors{
                         vsg::DescriptorImage::create(color_info, 0, 0, sampled),
                         vsg::DescriptorImage::create(general(nearest, ao_view), 1, 0, sampled),
                         vsg::DescriptorImage::create(general(linear, bloom_levels.front()), 2, 0,
                                                      sampled)})));
        // After VSG's view matrices, which it pushes into the first 128 bytes.
        state->add(vsg::PushConstants::create(fragment, 128, targets->composite_push));
        state->addChild(vsg::Draw::create(3, 1, 0, 0));
        auto camera = vsg::Camera::create(vsg::Perspective::create(), vsg::LookAt::create(),
                                          vsg::ViewportState::create(extent));
        targets->composite = vsg::View::create(camera, state, static_cast<vsg::ViewFeatures>(0));
    }

    targets_ = std::move(targets);
    buildFrameCommands();
}

void PostProcess::beginFrame(const glm::vec4& clear)
{
    views_.clear();
    for (auto& target : shadows_) target.views.clear();
    shadow_recorded_ = false;
    if (targets_) setClearValues(*targets_->scene, clear);
    buildFrameCommands();
}

vsg::ref_ptr<vsg::View> PostProcess::addView(vsg::ref_ptr<vsg::View> view)
{
    if (!targets_) return {};
    views_.push_back(view);
    auto& targets = *targets_;
    // The composite covers every scene view. Ambient occlusion reconstructs
    // positions with one projection, so it runs only when all views share a
    // viewport (the sky and the world); the world view, added last, decides it.
    VkRect2D bounds{};
    bool shared_viewport = true;
    const auto first = views_.front()->camera->viewportState->getViewport();
    for (const auto& scene_view : views_) {
        const auto viewport = scene_view->camera->viewportState->getViewport();
        shared_viewport = shared_viewport && viewport.x == first.x && viewport.y == first.y &&
                          viewport.width == first.width && viewport.height == first.height;
        const VkRect2D rect{
            {static_cast<std::int32_t>(viewport.x), static_cast<std::int32_t>(viewport.y)},
            {static_cast<std::uint32_t>(viewport.width),
             static_cast<std::uint32_t>(viewport.height)}};
        if (&scene_view == &views_.front()) {
            bounds = rect;
            continue;
        }
        const auto right =
            std::max(bounds.offset.x + static_cast<std::int32_t>(bounds.extent.width),
                     rect.offset.x + static_cast<std::int32_t>(rect.extent.width));
        const auto bottom =
            std::max(bounds.offset.y + static_cast<std::int32_t>(bounds.extent.height),
                     rect.offset.y + static_cast<std::int32_t>(rect.extent.height));
        bounds.offset.x = std::min(bounds.offset.x, rect.offset.x);
        bounds.offset.y = std::min(bounds.offset.y, rect.offset.y);
        bounds.extent = {static_cast<std::uint32_t>(right - bounds.offset.x),
                         static_cast<std::uint32_t>(bottom - bounds.offset.y)};
    }
    ambient_occlusion_ = settings_.ssao > 0.0f && shared_viewport;
    const auto projection = view->camera->projectionMatrix->transform();
    glm::mat4 matrix;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            matrix[column][row] = static_cast<float>(projection[column][row]);
    const std::array<std::int32_t, 4> rect{bounds.offset.x, bounds.offset.y,
                                           static_cast<std::int32_t>(bounds.extent.width),
                                           static_cast<std::int32_t>(bounds.extent.height)};
    writePushData(*targets.ssao->push,
                  PostParameters{matrix,
                                 rect,
                                 {settings_.ssao_radius, settings_.ssao_radius * 0.04f, 1.5f, 0}});
    writePushData(*targets.blur->push, PostParameters{matrix, rect, {}});
    writePushData(*targets.depth_copy->push, PostParameters{matrix, rect, {}});
    targets.depth_copy->size(bounds.extent.width, bounds.extent.height);
    targets.ssao->size(bounds.extent.width, bounds.extent.height);
    targets.blur->size(bounds.extent.width, bounds.extent.height);
    writePushData(*targets.composite_push,
                  // The upsample chain sums every pyramid level into the first.
                  CompositeParameters{
                      {settings_.exposure, settings_.ssao,
                       settings_.bloom / static_cast<float>(targets.bloom_down.size()),
                       std::clamp(settings_.shoulder, 0.0f, 0.99f)},
                      {settings_.saturation, settings_.contrast,
                       static_cast<float>(settings_.post_debug), ambient_occlusion_ ? 1.0f : 0.0f},
                      {g_render_gamma, 0, 0, 0}});
    targets.composite->camera->viewportState = vsg::ViewportState::create(
        bounds.offset.x, bounds.offset.y, bounds.extent.width, bounds.extent.height);
    buildFrameCommands();
    return views_.size() == 1 ? targets.composite : vsg::ref_ptr<vsg::View>{};
}

void PostProcess::buildFrameCommands()
{
    root_->children.clear();
    // The atlases are bound to every world view, so each is cleared into its
    // sampled layout once even before any view casts into it.
    for (auto& target : shadows_) {
        if (!target.graph || (target.views.empty() && shadow_ready_)) continue;
        target.graph->children.assign(target.views.begin(), target.views.end());
        root_->addChild(target.graph);
        shadow_recorded_ = true;
    }
    if (!targets_ || views_.empty()) return;
    auto& targets = *targets_;
    targets.scene->children.assign(views_.begin(), views_.end());
    root_->addChild(targets.prepare);
    root_->addChild(targets.scene);
    auto& compute = targets.compute;
    compute->children.clear();
    if (ambient_occlusion_) {
        compute->addChild(targets.depth_copy->commands);
        compute->addChild(computeToCompute());
        compute->addChild(targets.ssao->commands);
        compute->addChild(computeToCompute());
        compute->addChild(targets.blur->commands);
        compute->addChild(computeToCompute());
    }
    if (settings_.bloom > 0.0f) {
        for (auto& pass : targets.bloom_down) {
            compute->addChild(pass->commands);
            compute->addChild(computeToCompute());
        }
        for (auto& pass : targets.bloom_up) {
            compute->addChild(pass->commands);
            compute->addChild(computeToCompute());
        }
    }
    compute->addChild(vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        vsg::MemoryBarrier::create(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)));
    root_->addChild(compute);
}

} // namespace render
