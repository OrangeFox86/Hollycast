# HollyCast PostFX Shaders (v1)

This subsystem implements screen-space post-processing on the final frame.

## Scope

- Final-frame post-processing only
- GLSL fragment shaders only (v1). The vertex shader is fixed/fullscreen.
- Multi-pass chains supported via a simple INI preset format
- Fail-open behavior: if a preset fails to load or a shader fails to compile, HollyCast falls back to raw output

## Presets

Built-in presets live under `resources/shaders/postfx/presets/` and are embedded into the executable.

At runtime, they are referenced by resource paths like:

- `shaders/postfx/presets/scanlines.ini`
- `shaders/postfx/presets/sharpen_scanlines.ini`
- `shaders/postfx/presets/vibrance.ini`

External presets are discovered at startup from:

- `data/shaders/`

(`data/` is HollyCast's writable data directory on your platform.)
This folder tree is created automatically on startup if it does not already exist.

Supported external preset file types:

- `.ini` (multi-pass presets)
- `.glsl` (single-pass direct shader file)

## Preset INI Format

```ini
[Preset]
name = My Preset
passes = 2

[Pass0]
shader = shaders/postfx/sharpen.glsl
scale_type = viewport   ; viewport | source | absolute
scale_x = 1.0
scale_y = 1.0
filter = linear         ; linear | nearest

[Pass1]
shader = shaders/postfx/scanlines.glsl
filter = linear
```

## Shader Parameters

The loader parses `#pragma parameter` lines inside shader source, for example:

```glsl
#pragma parameter SHARPEN "Sharpen" 0.25 0.00 1.00 0.01
```

Those parameters appear in the Settings UI and are persisted per preset.

## Common Builtins

HollyCast provides a small set of common uniforms/aliases commonly used in community GLSL packs:

- `sampler2D Texture` (also aliased as `Source` and `Original`)
- `vec2 SourceSize`, `vec2 OutputSize` (pixels)
- `int FrameCount`, `float Time` (seconds)
- `vec2 vTexCoord` and `TEX0` (normalized UV)

## Notes

- This is intentionally not a full RetroArch preset graph implementation in v1.
- This is also intentionally not texture upscaling (xBRZ, etc). It is purely screen-space post-processing.
