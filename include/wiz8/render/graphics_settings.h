#pragma once

#include <string_view>

/* Renderer enhancements beyond the retail look. Every field defaults to the
   enhanced value; `WIZ8_GFX` overrides them as a comma-separated list such as
   `msaa=4,aniso=8,ssao=0`, and `WIZ8_GFX=retail` turns all of them off. */
struct GraphicsSettings {
    int msaa = 8;              // requested samples; the device's best supported count wins
    float anisotropy = 16.0f;  // 1 disables anisotropic filtering
    bool per_pixel_lighting = true;
    bool alpha_to_coverage = true;
    // The world renders into an HDR target and is resolved through post-processing.
    bool hdr = true;
    float light_max = 2.0f;     // ceiling for lit colours in the HDR target
    float ssao = 0.85f;         // ambient occlusion strength; 0 disables
    float ssao_radius = 1200.0f; // eye-space units: nearby ground is ~3000 from the eye
    float bloom = 1.0f;         // bloom intensity; 0 disables
    float bloom_threshold = 1.0f;  // only light above display white blooms
    float exposure = 1.0f;
    float saturation = 1.08f;
    float contrast = 1.04f;
    int post_debug = 0;         // 1 shows ambient occlusion, 2 bloom
    float shoulder = 0.8f;      // tone curve stays linear below this

    static GraphicsSettings retail();
    // Applies `name=value` items (and `retail`) from `spec` over this value.
    GraphicsSettings with(std::string_view spec) const;
};

// The process's settings: defaults overridden by the WIZ8_GFX environment variable.
const GraphicsSettings& CurrentGraphicsSettings();
