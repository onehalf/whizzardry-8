# Whizzardry 8: `engine/max` fork

This is a personal fork of [agluszak/whizzardry-8](https://github.com/agluszak/whizzardry-8).
The `engine/max` branch adds a modern renderer and fixes on top of upstream `main`.
Everything below the line is the upstream README, which still applies.

## What this fork does

Upstream keeps the original look of Wizardry 8. This fork asks a different question:
how good can the same world look with a modern renderer? The goal is **maximalist but
cohesive**: strong effects, tuned so the game still looks like Wizardry 8.

Done on `engine/max`:

- **Enhanced renderer**: 8x MSAA, 16x anisotropic filtering, alpha-to-coverage
  for foliage, per-pixel lighting.
- **HDR pipeline**: floating-point scene target, SSAO, bloom and a tone curve.
- **Sun shadows**: cascaded shadow maps from the sky's sun and moon.
- **Torch shadows**: cube shadow maps for the nearest baked level lights.
- **Brightness slider** in Options → Graphics.
- **Fixes** for sky far-clip, monster shadow stripes, HUD repaints, the game cursor,
  chest picker backdrops and level travel.

Planned (not done yet):

- Widescreen (Hor+) view with a UI layout made for wide screens.
- Detail textures, texture upscale and a sharp UI filter.
- Optional fan fixes from the community patches (1.261 and 1.28), off by default.
  Balance changes stay out.

## Upstream first

Bug fixes go upstream as small pull requests when they make sense there:

- [#667](https://github.com/agluszak/whizzardry-8/pull/667) chest picker backdrops
- [#668](https://github.com/agluszak/whizzardry-8/pull/668) game cursor
- [#669](https://github.com/agluszak/whizzardry-8/pull/669) HUD overlay repaints
- [#670](https://github.com/agluszak/whizzardry-8/pull/670) sky far-clip

The renderer work stays on this branch. Upstream can cherry-pick any part of it.
This fork's `main` mirrors upstream `main`; the `fix/*` branches are the PR heads.

## Graphics settings

The enhanced renderer is on by default. Set `WIZ8_GFX` to a comma-separated list to
change it, or use `retail` to get the original look:

```sh
WIZ8_ASSET_ROOT=/path/to/prepared-assets WIZ8_GFX=retail ./run.sh
WIZ8_ASSET_ROOT=/path/to/prepared-assets WIZ8_GFX=msaa=4,ssao=0,bloom=0 ./run.sh
```

Keys: `msaa`, `aniso`, `ppl`, `hdr`, `lightmax`, `ssao`, `ssao_radius`, `bloom`,
`bloom_threshold`, `exposure`, `saturation`, `contrast`, `shoulder`, `shadows`,
`shadow_size`, `shadow_distance`, `sun_tilt`, `light_shadows`, `shadow_lights`,
`light_shadow_size`, `debug` (1 = AO buffer, 2 = bloom buffer, 3 = shadow cascades).
See `src/render/graphics_settings.cpp` for defaults.

You still need your own retail copy of Wizardry 8. Build and run the same way as upstream (below).

---

# Whizzardry 8

Whizzardry 8 is a work-in-progress port of [Wizardry 8](https://en.wikipedia.org/wiki/Wizardry_8) to modern, 64-bit Linux, macOS and Windows.

It builds on the recovered source code from [Wizardry 8 Decomp](https://github.com/agluszak/wizardry-8-decomp), but has a different goal: **keep the game, replace the outdated machinery**. Instead of reproducing the original executable, we're simplifying the code and using C++26, SDL3 and other cross-platform libraries.

Linux is the only platform currently tested in CI. macOS and Windows builds are not yet verified, and the game is still under development.

This repository does **not** include retail game assets. You'll need your own retail copy of Wizardry 8.

## Build and play (Linux)

Install Docker and run:

```sh
./build.sh
```

The first run pulls the published image pinned by digest in `toolchain-image.txt`
and resolves the pinned Conan dependencies. Subsequent builds reuse that image,
Conan binaries and ccache. CI uses the same entry point and image; ordinary builds
never install system packages. The host does not need Clang, CMake or Conan.

The Toolchain image workflow publishes compiler and system-dependency changes.
After it succeeds, update `toolchain-image.txt` with the digest from its
`toolchain-image` artifact and validate the consumers before merging.
The image contains no game assets. A missing system dependency is an image
problem: fix the Dockerfile rather than enabling Conan package installation.

Prepare assets from a retail installation once:

```sh
./run.sh --prepare-assets /path/to/prepared-assets /path/to/retail-install
```

Choose a new directory outside the retail installation, then play:

```sh
WIZ8_ASSET_ROOT=/path/to/prepared-assets ./run.sh
```

Conan runtime paths are activated automatically by the launcher. The game runs
on the host without Docker, provided its C++ runtime and Vulkan driver are
compatible with the build environment. Saves, settings and logs use the
per-user application directory; override it with `WIZ8_USER_ROOT`.

Use `./build.sh linux-ci` for the assertion-enabled, optimized Debug CI build, `./build.sh linux-dev`, `./build.sh linux-asan` or
`./build.sh linux-integer-audit` for other configurations, and set
`WIZ8_BUILD_DIR` to select the resulting executable in `run.sh`.
`./build.sh -- <command>` runs a command in the same build container.
macOS and Windows builds are not yet verified.

To try an imported, interactive VSG object in a retail world:

```bash
WIZ8_ASSET_ROOT=/path/to/prepared-assets ./run.sh /WINDOW /NATIVEOBJECT=examples/native/prism.vsgt
```

Start or load a game. A prism appears just ahead of the party at level entry.
Point at it with an empty hand and click to toggle its glow; the normal message
log describes the action. It uses the world's lights and fog. This optional sample
is recreated at each level entry, with no changes to retail assets or save records.
The same option accepts other static indexed VSG position/normal/UV geometry.


## Contributing

This is a modernization project, not an exercise in preserving the original engine's structure. We aim to remove obsolete code and unnecessary abstractions while keeping Wizardry 8's gameplay, content and file compatibility.

For the current technical direction, see [AGENTS.md](AGENTS.md), [the architecture](ARCHITECTURE.md) and [the roadmap](ROADMAP.md).

After building, run tests in the same environment as CI:

```sh
./build.sh -- bash -c 'source build/linux/conan/conanrun.sh; ctest --preset linux'
```

Graphics tests and runtime scenarios render through SDL's offscreen video driver, so
they need no display, open no windows and receive no desktop input. CI also enables
the Khronos validation layer and fails graphics tests on validation errors.

Some runtime tests also need prepared retail assets:

```sh
./build.sh -- bash -c 'source build/linux/conan/conanrun.sh; ctest --preset linux -L runtime'
```

A failing scenario saves the game's frame under `tests/runtime-artifacts/` in the
build tree; [the runtime fixture notes](tests/runtime/fixtures/README.md) also
describe the golden screen signatures and how to update them.

C and C++ changes should use the repository's `.clang-format` configuration (clang-format 23).

## Diagnostics

Linux uses Clang 23 with GCC 16's libstdc++ in C++26 mode. CMake checks for
`std::function_ref` and `std::inplace_vector` during configuration; upgrading
only the language flag is insufficient. macOS and Windows also need a standard
library implementing both facilities and remain unverified.

`linux-asan` combines ASan and UBSan with fatal undefined-behavior diagnostics.
Debug and RelWithDebInfo enable `_GLIBCXX_ASSERTIONS`; ordinary assertions follow
the build configuration. Signed overflow and strict aliasing follow the
language rules (no `-fwrapv` or `-fno-strict-aliasing`), so UBSan's
signed-overflow check guards gameplay arithmetic; retail wraparound is written
as explicit unsigned arithmetic. Plain `char` holds only text; small signed
numbers use `signed char` or `std::int8_t`, so no build sets `-fsigned-char`.

The normal tidy allowlist includes assignment-in-condition, string-view C API
misuse, allocation ownership, dangling string buffers and last-use moves.
Configure the analysis preset and run its checks inside the same container:

```sh
./build.sh linux-tidy --configure-only
./build.sh -- bash -c 'source build/linux-tidy/conan/conanbuild.sh; cmake --build build/linux-tidy --target wiz8_shader_headers'
./build.sh -- bash -c 'CLANG_TIDY=clang-tidy-23 RUN_CLANG_TIDY=run-clang-tidy-23 tools/run-clang-tidy.sh build/linux-tidy'
./build.sh -- python3 tools/run-diagnostic-audit.py build/linux-tidy
```

The separate audit checks unchecked optional dereferences (intentional `.value()`
exceptions are allowed), moved-from ownership, strict lifetime safety, exhaustive
enum handling and buffer use at parser/image boundaries. Experimental compiler
warnings are retained in `diagnostic-audit.log` for review. The optional-access
and moved-from checks fail on findings. Weekly and manually dispatched CI runs
include these audits and `linux-integer-audit`, which adds implicit truncation
instrumentation to file/image boundaries. The ordinary ASan/UBSan jobs run on
every PR. `-Wconversion` and `-Wsign-conversion` remain outside the global warning
baseline, and `-fstrict-bool` is not enabled.

`function_ref` is used only for borrowed synchronous callbacks; stored callbacks
retain ownership. Fixed-capacity storage requires a proven bound. SIMD and hive
need workload evidence before adoption; reflection and contracts remain limited
by compiler support.

CI reuses successful tidy results only after Clang rediscovers the current
include graph and the contents of every dependency match, including system
headers. Tool versions, compile commands and tidy configuration also invalidate
the cache. Failed scans and analyses are always retried; clean files remain
cached when another file fails. To use this locally:

```sh
CLANG_TIDY_CACHE_DIR="$PWD/build/tidy-cache" ./build.sh -- bash -c \
    'CLANG_TIDY=clang-tidy-23 RUN_CLANG_TIDY=run-clang-tidy-23 tools/run-clang-tidy.sh build/linux-tidy'
```

Unset `CLANG_TIDY_CACHE_DIR` for an uncached run. CI prints hit/miss counts and
elapsed times, skips Native and Format for documentation-only changes, and regenerates
offline shaders when their sources, tools, dependencies or build settings change.
Weekly/manual runs retain the full source scope; their integer-sanitizer job
includes ASan and UBSan, replacing the duplicate ordinary sanitizer run.

Analysis runs in two stable partitions with a direct check for each; there are no
separate scope or aggregation jobs. Header dependency discovery stays within each
partition. A manual Native run always runs the checks; its `full_scan` option also
bypasses the tidy cache for regular and ownership checks. Manual runs include the
compiler audit and integer-sanitizer build.
