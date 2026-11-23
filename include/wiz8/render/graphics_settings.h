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

    static GraphicsSettings retail();
    // Applies `name=value` items (and `retail`) from `spec` over this value.
    GraphicsSettings with(std::string_view spec) const;
};

// The process's settings: defaults overridden by the WIZ8_GFX environment variable.
const GraphicsSettings& CurrentGraphicsSettings();
