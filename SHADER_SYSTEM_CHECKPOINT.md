<!-- Portions Copyright 2026 The Hollycast Authors -->

# Shader System Implementation Checkpoint

Checkpoint: 2026-07-13
Branch: `Shadr-Systm-Implementation`
State: local work only; nothing staged, committed, pushed, or stashed.

## Completed before this checkpoint

- Phases 1 through 4: shared ordered Slang chains, safe package-root Slang includes, GL/Vulkan/DX11 pass execution, scaling/filtering/mipmaps/frame modulo, LUTs, aliases, feedback, typed parameters, and portable custom Slang vertex stages.
- Windows `flycast` build passed after the checkpoint changes below.

## Phase 5 through 9 work completed in this pass

- Replaced two-frame pass history with indexed history rings on GL, Vulkan, and DX11. Names such as `FirstHistory2` now request two completed frames ago.
- Added Vulkan and DX11 source-history rings; Vulkan source history is copied before chain execution.
- Added matrix parameter types and backend binding support for 2x2, 3x3, and 4x4 values.
- Added persistent scalar parameter configuration plus an explicit Reload Shader action.
- Added same-path reload generation so edited packages can be recompiled without changing the selected path.
- Added the clean-room ReShade compiler source and wired typed uniforms, structs, helper functions, control flow source, samplers, texture aliases, custom vertex entry points, and ordered technique passes into chain loading.
- Added ReShade package LUT discovery from texture `source` annotations.
- Added typed FX annotations, so standard forms such as `string source = "color"` resolve correctly.
- Cached validated pass sources in the shared chain. GL, Vulkan, and DX11 now compile exactly the source that validation expanded and reflected, without a second file read or include expansion.
- Confirmed renderer teardown recreates each backend chain with the device/context lifecycle; a new device reloads the selected package through its normal generation path.
- Added true ordered MRT graph support: ReShade `RenderTarget0` through `RenderTarget7` are reflected as named outputs, later passes select an exact producer attachment, and GL/GLES3, Vulkan, and DX11 allocate, render, history-route, and blend each attachment.
- Preserved each ReShade render target's declared `RGBA8`, `RGBA8_SRGB`, or `RGBA16F` format instead of applying one pass-wide format to every attachment.
- Added GL per-output mip-chain generation, GL/GLES3 draw-buffer capability checks, and Vulkan checks for both fragment-output limits and linear-blit mip support.
- Preserved typed vector and matrix parameter values through configuration, the UI, and GL/Vulkan/DX11 runtime binding. Values serialize as comma-separated components while scalar settings remain backward compatible.
- Added named ReShade technique discovery and selection through rend.PostProcessShaderTechnique, with a settings combo and renderer reload generation.
- Added ReShade object-like and function-like macro expansion plus compound #if/#elif expressions with defined, comparisons, arithmetic, and boolean operators.
- Added ReShade subtract, reverse-subtract, minimum, and maximum blend operations across GL, Vulkan, and DX11.
- Added explicit Slang layout(set=..., binding=...) metadata retention for standard resource declarations and expanded custom vertex aliases for common Position/TexCoord names.
- Added a versioned, mtime/size keyed package source cache under the writable Flycast config directory. Cache corruption or stale metadata falls back to the source file.
- Added common RGBA16_UNORM and RGBA32_FLOAT target format mappings across all three backends.

## Exact remaining work

1. The FX path is still a clean-room compiler, not a complete HLSL/FX implementation. It now handles substantially more preprocessing and blend state, but full overload/type/resource/control-flow lowering still needs a real backend-neutral expression IR.
2. Slang sampler arrays, cube/3D resource execution, and arbitrary stage interfaces still need backend descriptor/view support; explicit binding metadata is retained and validated but not yet used to override each backend's safe graph bindings.
   Standard Slang push-constant blocks, MVP uniform blocks, and custom vertex varyings also still need backend-neutral lowering before packages such as the upstream scanline preset can be claimed as supported on every backend.
3. The source package cache is persistent and shared before compilation, but compiled GL/DX11 binaries are still driver-owned and require backend-specific disk caches keyed by driver and shader source.
4. Run the focused unit target once the unrelated local curl/DX9 test dependencies are available, then perform live GPU validation on GL/GLES, Vulkan, and DX11 plus release builds on Windows, Android, Linux, and macOS.

## Resume point

Start with a backend-neutral FX expression IR and Slang descriptor-array lowering. Do not claim full ReShade compatibility until the compiler can lower the required FX language constructs. Preserve core/deps/gamesdk/; it is unrelated user work.

Validation: `cmake --build BUILD-WINDOWS-DEV --target flycast --parallel 4` passed after the final GL/Vulkan capability and format changes. The focused `G:\tmp\shader_compiler_smoke.exe` and `G:\tmp\shader_preprocess_smoke.exe` passed with conditional preprocessing, function-like macros, typed annotations, helpers, control flow, and two reflected FX outputs. `git diff --check` reports no whitespace errors; only normal Git LF/CRLF conversion warnings remain. The first 16-way attempt exhausted MSVC heap in unrelated `dx11_renderstate.cpp`; the lower-parallel retry completed and linked `flycast.exe`.
