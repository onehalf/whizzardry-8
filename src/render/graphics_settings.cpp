#include "wiz8/render/graphics_settings.h"

#include <charconv>
#include <cstdio>
#include <string>

#include <SDL3/SDL_stdinc.h>

namespace {
bool parseBool(std::string_view value)
{
    return !(value == "0" || value == "off" || value == "false" || value == "no");
}

float parseFloat(std::string_view value, float fallback)
{
    float result = fallback;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}
} // namespace

GraphicsSettings GraphicsSettings::retail()
{
    return {.msaa = 1, .anisotropy = 1.0f, .per_pixel_lighting = false, .alpha_to_coverage = false};
}

GraphicsSettings GraphicsSettings::with(std::string_view spec) const
{
    GraphicsSettings result = *this;
    while (!spec.empty()) {
        const auto comma = spec.find(',');
        const auto item = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        if (item.empty()) continue;
        if (item == "retail") {
            result = retail();
            continue;
        }
        const auto equals = item.find('=');
        const auto name = item.substr(0, equals);
        const auto value = equals == std::string_view::npos ? "1" : item.substr(equals + 1);
        if (name == "msaa")
            result.msaa = static_cast<int>(parseFloat(value, static_cast<float>(result.msaa)));
        else if (name == "aniso")
            result.anisotropy = parseFloat(value, result.anisotropy);
        else if (name == "ppl")
            result.per_pixel_lighting = parseBool(value);
        else if (name == "a2c")
            result.alpha_to_coverage = parseBool(value);
        else
            std::fprintf(stderr, "WIZ8_GFX: unknown setting '%.*s'\n",
                         static_cast<int>(name.size()), name.data());
    }
    return result;
}

const GraphicsSettings& CurrentGraphicsSettings()
{
    static const GraphicsSettings settings = [] {
        const char* spec = SDL_getenv("WIZ8_GFX");
        return GraphicsSettings{}.with(spec ? spec : "");
    }();
    return settings;
}
