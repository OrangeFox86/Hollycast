/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once

#include "slang_frontend.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace postprocess {

// Bound reflected controls to keep UI and upload costs predictable. Vulkan uses
// a uniform buffer for these values, so this is no longer constrained by the
// 128-byte minimum push-constant guarantee.
inline constexpr uint32_t MaxShaderParameterComponents = 256;
inline constexpr size_t MaxShaderSourceBytes = 8u * 1024u * 1024u;
inline constexpr size_t MaxExpandedShaderSourceBytes = 32u * 1024u * 1024u;
inline constexpr size_t MaxShaderLookupTextures = 64;
inline constexpr size_t MaxShaderLookupFileBytes = 64u * 1024u * 1024u;
inline constexpr int MaxShaderTargetDimension = 16384;
inline constexpr uint64_t MaxShaderChainTargetBytes = 512ull * 1024ull * 1024ull;

enum class ShaderFormat {
	Unknown,
	GLSL,
	GlslPreset,
	ReShadeFX,
	Slang,
	SlangPreset,
};

enum class ScaleType {
	Source,
	Viewport,
	Absolute,
};

// These requests describe a render target without exposing an API-specific
// image format. Backends may reject a request they cannot allocate, but must
// not silently substitute a different color-space or precision.
enum class ShaderTargetFormat {
	Default,
	Float,
	Srgb,
};

enum class ShaderWrapMode {
	ClampToEdge,
	ClampToBorder,
	Repeat,
	MirroredRepeat,
};

enum class ShaderInputKind {
	Source,
	Original,
	PassOutput,
	History,
	Feedback,
	LookupTexture,
};

enum class ShaderBlendFactor {
	Zero,
	One,
	SourceColor,
	OneMinusSourceColor,
	DestinationColor,
	OneMinusDestinationColor,
	SourceAlpha,
	OneMinusSourceAlpha,
	DestinationAlpha,
	OneMinusDestinationAlpha,
};

enum class ShaderBlendOperation {
	Add,
	Subtract,
	ReverseSubtract,
	Minimum,
	Maximum,
};

constexpr uint32_t InvalidShaderPass = static_cast<uint32_t>(-1);
constexpr size_t MaxShaderLayers = 8;

struct ShaderPresetTexture {
	std::string name;
	std::string path;
	bool linearFilter = false;
	bool useMipmaps = false;
	ShaderWrapMode wrapMode = ShaderWrapMode::ClampToEdge;
};

// A resolved input remains symbolic until a renderer binds it. sourcePass is
// set only for pass outputs, histories, and feedback targets.
struct ShaderPassInput {
	std::string bindingName;
	ShaderInputKind kind = ShaderInputKind::Source;
	std::string resourceName;
	uint32_t sourcePass = InvalidShaderPass;
	// Attachment zero remains the default so existing Slang aliases retain their
	// meaning while named MRT targets can be sampled independently.
	uint32_t sourceOutput = 0;
	uint32_t historyOffset = 1;
};

struct ShaderPassSampler {
	std::string bindingName;
	bool linearFilter = false;
	bool useMipmaps = false;
	ShaderWrapMode wrapMode = ShaderWrapMode::ClampToEdge;
};

// Every pass has a primary output. Additional outputs are explicit so a
// backend can allocate compatible attachments without inferring their format
// from unrelated sampler declarations.
struct ShaderPassOutput {
	std::string alias;
	ShaderTargetFormat targetFormat = ShaderTargetFormat::Default;
	std::string targetFormatName;
	bool useMipmaps = false;
};

struct ShaderParameter {
	std::string name;
	// Multi-layer stacks may expose the same shader uniform more than once.
	// key is the stable UI/config identity while name remains the real binding.
	std::string key;
	std::string label;
	float value = 0.f;
	float minimum = 0.f;
	float maximum = 0.f;
	float step = 0.f;
	// Rich Slang/FX values are retained here while value remains the legacy
	// scalar projection used by older callers.
	std::array<float, 16> values { 0.f };
	std::array<float, 16> minimumValues { 0.f };
	std::array<float, 16> maximumValues { 0.f };
	std::array<float, 16> stepValues { 0.f };
	uint32_t componentCount = 1;
	SlangParameterType type = SlangParameterType::Float;
	// ReShade combo/radio annotations expose named integer choices. An empty
	// list means the parameter uses its declared numeric range instead.
	std::vector<std::string> choiceLabels;
	// Preset values are user-authored overrides, not reflected defaults. Keep
	// that distinction while later passes contribute the parameter metadata.
	bool hasPresetValue = false;
	uint32_t layerIndex = 0;
};

struct ShaderPass {
	std::string sourcePath;
	// Every resolved file must remain below this package root. It is retained on
	// the pass so recursive Slang includes cannot escape through a nested file.
	std::string packageRoot;
	std::string alias;
	uint32_t layerIndex = 0;
	ShaderFormat format = ShaderFormat::Unknown;
	ScaleType scaleTypeX = ScaleType::Source;
	ScaleType scaleTypeY = ScaleType::Source;
	float scaleX = 1.f;
	float scaleY = 1.f;
	bool linearFilter = false;
	bool useMipmaps = false;
	uint32_t frameCountModulo = 0;
	ShaderTargetFormat targetFormat = ShaderTargetFormat::Default;
	std::string targetFormatName;
	bool feedbackTarget = false;
	bool blendEnabled = false;
	ShaderBlendFactor sourceBlend = ShaderBlendFactor::One;
	ShaderBlendFactor destinationBlend = ShaderBlendFactor::Zero;
	ShaderBlendOperation blendOperation = ShaderBlendOperation::Add;
	// Ordered UI layers can switch from the raw renderer image to display space
	// once. Marking the boundary on the pass preserves mixed Slang/display stacks.
	bool composeDisplayInput = false;
	// Empty means one implicit color output named by alias. ReShade pass states
	// populate this with their RenderTargetN texture names.
	std::vector<ShaderPassOutput> outputs;
	std::vector<ShaderPassInput> inputs;
	// ReShade permits different sampler state for bindings that reference the
	// same texture. Keep it per binding so every backend can preserve that state.
	std::vector<ShaderPassSampler> samplers;
	// ReShade effects can expose multiple ordered passes from one source file.
	uint32_t effectTechnique = InvalidShaderPass;
	uint32_t effectPass = InvalidShaderPass;
};

struct ShaderTechniqueGroup {
	size_t layerIndex = 0;
	std::string label;
	std::vector<std::string> techniques;
	int selectedTechnique = -1;
};

// This is deliberately backend-neutral: it gives every renderer the same
// size and frame inputs without leaking an API texture handle into the chain.
struct ShaderFrameInfo {
	int sourceWidth = 0;
	int sourceHeight = 0;
	int viewportWidth = 0;
	int viewportHeight = 0;
	uint32_t frameCount = 0;
	// Rotation remains a presentation operation for unscaled Slang chains. A
	// backend must not pre-scale an unrotated source into rotated dimensions.
	bool rotationPending = false;
};

struct ShaderChainStatus {
	std::string activePath;
	std::string error;
	std::vector<ShaderParameter> parameters;
	std::vector<std::string> techniques;
	int selectedTechnique = -1;
};

struct SlangShaderSource {
	std::string vertex;
	std::string fragment;
	std::vector<ShaderParameter> parameters;
};

struct ShaderPassSource {
	// The original stage and resource metadata are retained so the render graph
	// can validate bindings before a backend chooses how to compile them.
	std::string vertex;
	std::string fragment;
	std::string displayName;
	std::string targetFormatName;
	// Fragment outputs are counted during lowering so state blocks and backend
	// attachments cannot silently disagree about an MRT pass.
	uint32_t outputCount = 1;
	// RetroArch GLSL uses a vec4 fullscreen position, while HollyCast's native
	// portable vertex contract exposes the underlying vec3 attribute.
	bool retroArchGlsl = false;
	// DuckStation's legacy GLSL dialect receives a host-provided helper API and
	// keeps native input sizing even after the display-composition boundary.
	bool duckStationGlsl = false;
	std::vector<ShaderParameter> parameters;
	std::vector<SlangParameter> slangParameters;
	std::vector<SlangResource> slangResources;
};

struct ShaderChain {
	std::string packagePath;
	std::vector<ShaderPass> passes;
	std::vector<ShaderParameter> parameters;
	std::vector<ShaderPresetTexture> textures;
	std::vector<std::string> techniques;
	int selectedTechnique = -1;
	std::vector<ShaderTechniqueGroup> techniqueGroups;
	// Source is parsed once during validation, then shared by every backend
	// compiler. Reload explicitly invalidates this cache through a new chain.
	std::vector<ShaderPassSource> sources;
	uint32_t feedbackPass = InvalidShaderPass;
	// Slang presets define their own source/viewport scaling. Display GLSL and
	// ReShade effects instead receive an already display-sized input.
	bool wantsUnscaledInput = false;
};

ShaderFormat detectFormat(const std::string& path);
const char *formatName(ShaderFormat format);

// Resolves one pass target before a backend allocates it. Fractional sizes are
// rounded to the nearest pixel so all backends use identical chain geometry.
bool resolveTargetSize(const ShaderPass& pass, const ShaderFrameInfo& frame,
		int& width, int& height, std::string& error);
bool accumulateShaderTargetBytes(int width, int height, ShaderTargetFormat format,
		const std::string& formatName, bool mipmapped, uint32_t copies,
		uint64_t& totalBytes, std::string& error);
bool validateShaderLookupTexture(int width, int height, std::string& error);

// A normal display chain sees the composed presentation image. Presets that
// explicitly own source/viewport scaling retain the raw input and leave the
// presentation transform for the renderer after the chain.
bool shouldComposeDisplayInput(const ShaderFrameInfo& frame, bool wantsUnscaledInput);

// Resolves reflected Slang resources to the chain's symbolic inputs. This is
// intentionally renderer-free so loaders and backend compilers enforce the
// same alias ordering and feedback contract.
bool resolveShaderPassInputs(ShaderChain& chain, size_t passIndex,
		const ShaderPassSource& source, std::string& error);

// Backends publish a copy so UI code can report a rejected candidate without
// reaching into renderer-specific state or racing the render thread.
ShaderChainStatus getShaderChainStatus();
// Changes only when the published status or its parameter values change.
// Renderers use this to avoid copying UI-facing vectors on every frame.
uint64_t shaderChainStatusGeneration();
void reportShaderChainStatus(const std::string& activePath, const std::string& error,
		const std::vector<ShaderParameter>& parameters = {},
		const std::vector<std::string>& techniques = {}, int selectedTechnique = -1);
bool setShaderChainParameter(const std::string& name, float value);
bool setShaderChainParameterComponents(const std::string& name, const float *values, uint32_t componentCount);
const std::string& shaderParameterKey(const ShaderParameter& parameter);
void requestShaderChainReload();
uint64_t shaderChainReloadGeneration();

// Config stores parameter overrides as a compact name=value list. Invalid or
// stale entries are ignored so changing packages cannot poison activation.
void applyShaderChainParameterValues(ShaderChain& chain, const std::string& values);
std::string serializeShaderChainParameterValues(const std::vector<ShaderParameter>& parameters);
std::string serializeShaderTechniqueSelections(const std::vector<ShaderTechniqueGroup>& groups);

// Parses preset documents without touching storage. This keeps validation
// deterministic and lets import/UI code report malformed presets before activation.
bool parseSlangPreset(const std::string& presetText, const std::string& presetPath,
		ShaderChain& chain, std::string& error);
bool parseGlslPreset(const std::string& presetText, const std::string& presetPath,
		ShaderChain& chain, std::string& error);

// Lowers the common RetroArch combined VERTEX/FRAGMENT GLSL contract into the
// backend-neutral interface consumed by all three executors.
bool parseGlslShader(const std::string& shaderText, ShaderPassSource& source, std::string& error);

// Compatibility projection for callers that use the portable float-only
// parameter contract. New code should use parseSlangFrontend() for rich Slang
// metadata. Include expansion remains separate to enforce package-root rules.
bool parseSlangShader(const std::string& shaderText, SlangShaderSource& shader, std::string& error);

// Loads and validates a .slangp preset from HollyCast's platform-neutral storage.
bool loadSlangPreset(const std::string& presetPath, ShaderChain& chain, std::string& error);

// Returns the output mip-chain requirements for every pass. This keeps target
// allocation and generation identical across the renderer backends.
std::vector<std::vector<bool>> resolveShaderPassOutputMipmaps(const ShaderChain& chain);

// Builds a chain from a selected GLSL, GLSL preset, Slang, Slang preset, or ReShade FX file
// and reflects the parameters that its backend-neutral source exposes.
bool loadShaderChain(const std::string& path, ShaderChain& chain, std::string& error);
// Independent files selected in the UI are appended as ordered layers. Pass
// references are rebased once here so every renderer executes the same graph.
bool loadShaderChain(const std::vector<std::string>& paths, ShaderChain& chain, std::string& error);

// Reads the new ordered selection while preserving existing single-shader
// configurations created before the Shader page was introduced.
std::vector<std::string> configuredShaderPaths();
std::string shaderSelectionLabel(const std::vector<std::string>& paths);

// Loads one pass into HollyCast's portable fragment-body contract. Shader
// includes are expanded only within the pass package; FX is lowered by the
// clean-room, deliberately small compatibility compiler.
bool loadShaderPassSource(const ShaderPass& pass, ShaderPassSource& source, std::string& error);

// Lowers the supported ReShade FX subset to the same portable fragment-body
// contract used by the GLSL executor. Unsupported effects are rejected before
// activation rather than producing backend-specific behavior.
bool parseReShadeFx(const std::string& effectText, ShaderPassSource& source, std::string& error);
bool compileReShadeFxPass(const std::string& effectText, size_t techniqueIndex, size_t passIndex,
		ShaderPassSource& source, std::string& error);

// Translates the portable GLSL fragment-body contract to HLSL used by DX11.
// The intentionally narrow subset rejects declarations and texture sources
// that HollyCast cannot bind consistently across backends.
bool translateGlslFragmentToHlsl(const std::string& glslSource, std::string& hlslSource, std::string& error);
bool translateGlslFragmentToHlsl(const std::string& glslSource, const std::vector<SlangParameter>& parameters,
		std::string& hlslSource, std::string& error, uint32_t outputCount = 1);
bool translateGlslFragmentToHlsl(const std::string& glslSource, const std::vector<ShaderParameter>& parameters,
		std::string& hlslSource, std::string& error);
bool translateGlslVertexToHlsl(const std::string& glslSource, const std::vector<SlangParameter>& parameters,
		std::string& hlslSource, std::string& error);

// Translates a fully resolved render-graph pass, including named texture
// resources and GLSL sampler parameters, to Shader Model 4 compatible HLSL.
bool translateGlslGraphFragmentToHlsl(const ShaderPassSource& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		uint32_t outputCount, std::string& hlslSource, std::string& error);
bool translateGlslGraphVertexToHlsl(const std::string& vertexSource,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		std::string& hlslSource, std::string& error);

} // namespace postprocess
