#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/vec4.hpp>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/vk/Device.h>

struct GraphicsSettings;

namespace render {

// The depth atlases shadow views draw into: sun cascades, and torch cube faces.
enum class ShadowAtlas : std::uint8_t { Sun, Light };

/* The HDR world pipeline. Scene views (sky and world) render into a
   multisampled floating-point target; compute passes derive ambient occlusion
   from its depth and a bloom pyramid from its colour; a composite view in the
   window pass tone-maps the result into the world viewport, under the UI.

   Its frame commands run before the window render graph in each command
   graph that presents or captures the frame. */
class PostProcess {
public:
    PostProcess(vsg::ref_ptr<vsg::Device> device, VkSampleCountFlagBits samples,
                VkFormat depth_format, const GraphicsSettings& settings);
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // Rebuilds every target for the window's extent; a no-op when unchanged.
    void resize(VkExtent2D extent);
    // Commands recorded ahead of the window render graph.
    vsg::ref_ptr<vsg::Node> commands() const { return root_; }
    // Starts a frame without scene views.
    void beginFrame(const glm::vec4& clear);
    // Adds a scene view. The first view of a frame returns the composite view
    // the caller places in the window pass; later views return null.
    vsg::ref_ptr<vsg::View> addView(vsg::ref_ptr<vsg::View> view);
    // Called once a frame's commands have been submitted.
    void frameSubmitted();
    // Adds a view that draws into a shadow atlas this frame.
    void addShadowView(ShadowAtlas atlas, vsg::ref_ptr<vsg::View> view);
    // The sampled shadow atlas, or null when those shadows are off.
    vsg::ref_ptr<vsg::ImageInfo> shadowMap(ShadowAtlas atlas) const
    {
        return shadows_[static_cast<std::size_t>(atlas)].info;
    }

private:
    struct Targets;
    void buildFrameCommands();

    vsg::ref_ptr<vsg::Device> device_;
    VkSampleCountFlagBits samples_;
    VkFormat depth_format_;
    const GraphicsSettings& settings_;
    vsg::ref_ptr<vsg::Group> root_ = vsg::Group::create();
    std::unique_ptr<Targets> targets_;
    std::vector<vsg::ref_ptr<vsg::View>> views_;
    // One depth atlas per kind of shadow; both draw through one render pass.
    struct ShadowTarget {
        std::vector<vsg::ref_ptr<vsg::View>> views;
        vsg::ref_ptr<vsg::RenderGraph> graph;
        vsg::ref_ptr<vsg::ImageInfo> info;
    };
    std::array<ShadowTarget, 2> shadows_;
    bool shadow_ready_ = false;
    bool shadow_recorded_ = false;
    bool ambient_occlusion_ = false;
};

} // namespace render
