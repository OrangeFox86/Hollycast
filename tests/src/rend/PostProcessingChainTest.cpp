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
#include "rend/postprocess/shader_chain.h"
#include "rend/postprocess/reshade_fx.h"
#if defined(USE_VULKAN)
#include "rend/vulkan/compiler.h"
#include "rend/vulkan/vulkan_shader_chain.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>

namespace {

bool compileHlsl(const std::string& source, const char *target, std::string& error)
{
	using CompileFunction = HRESULT (WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
			LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
	const HMODULE compilerModule = LoadLibraryW(L"d3dcompiler_47.dll");
	if (compilerModule == nullptr) {
		error = "d3dcompiler_47.dll is unavailable.";
		return false;
	}
	const auto compiler = reinterpret_cast<CompileFunction>(GetProcAddress(compilerModule, "D3DCompile"));
	ID3DBlob *shader = nullptr;
	ID3DBlob *diagnostics = nullptr;
	const HRESULT result = compiler == nullptr ? E_NOINTERFACE : compiler(source.data(), source.size(), nullptr,
			nullptr, nullptr, "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &diagnostics);
	if (FAILED(result))
		error = diagnostics != nullptr ? static_cast<const char *>(diagnostics->GetBufferPointer())
				: "D3DCompile failed without diagnostics.";
	if (diagnostics != nullptr)
		diagnostics->Release();
	if (shader != nullptr)
		shader->Release();
	FreeLibrary(compilerModule);
	return SUCCEEDED(result);
}

} // namespace
#endif

TEST(PostProcessingChainTest, ParsesSlangPresetPassesAndParameters)
{
	const std::string preset =
		"shaders = 2\n"
		"shader0 = shaders/scanlines.slang\n"
		"filter_linear0 = true\n"
		"scale_type0 = viewport\n"
		"scale_x0 = 2.0\n"
		"shader1 = shaders/blur.slang\n"
		"scale_type_x1 = source\n"
		"scale_type_y1 = absolute\n"
		"scale_x1 = 1.5\n"
		"scale_y1 = 720\n"
		"frame_count_mod1 = 4\n"
		"parameters = SCANLINE; BLOOM\n"
		"SCANLINE = 0.75\n"
		"BLOOM = 0.25\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangPreset(preset, "C:/Shaders/crt.slangp", chain, error)) << error;
	EXPECT_TRUE(chain.wantsUnscaledInput);
	ASSERT_EQ(2u, chain.passes.size());
	EXPECT_EQ(postprocess::ShaderFormat::Slang, chain.passes[0].format);
	EXPECT_TRUE(chain.passes[0].linearFilter);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[0].scaleTypeX);
	EXPECT_EQ(postprocess::ScaleType::Absolute, chain.passes[1].scaleTypeY);
	EXPECT_EQ(4u, chain.passes[1].frameCountModulo);
	ASSERT_EQ(2u, chain.parameters.size());
	EXPECT_EQ("SCANLINE", chain.parameters[0].name);
	EXPECT_FLOAT_EQ(0.25f, chain.parameters[1].value);
}

TEST(PostProcessingChainTest, RejectsUnsupportedPassFormat)
{
	postprocess::ShaderChain chain;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangPreset("shaders = 1\nshader0 = pass.glsl\n",
			"C:/Shaders/crt.slangp", chain, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, ParsesRetroArchGlslPreset)
{
	const std::string preset =
		"shaders = 2\n"
		"shader0 = passes/first.glsl\n"
		"filter_linear0 = true\n"
		"scale_type0 = source\n"
		"shader1 = passes/final.glsl\n"
		"scale_type1 = viewport\n"
		"frame_count_mod1 = 8\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslPreset(preset, "C:/Shaders/crt.glslp", chain, error)) << error;
	EXPECT_TRUE(chain.wantsUnscaledInput);
	ASSERT_EQ(2u, chain.passes.size());
	EXPECT_EQ(postprocess::ShaderFormat::GLSL, chain.passes[0].format);
	EXPECT_NE(std::string::npos, chain.passes[0].sourcePath.find("passes/first.glsl"));
	EXPECT_TRUE(chain.passes[0].linearFilter);
	EXPECT_EQ(postprocess::ScaleType::Viewport, chain.passes[1].scaleTypeX);
	EXPECT_EQ(8u, chain.passes[1].frameCountModulo);
	EXPECT_EQ(postprocess::ShaderFormat::GlslPreset, postprocess::detectFormat("crt.glslp"));

	EXPECT_FALSE(postprocess::parseGlslPreset("shaders = 1\nshader0 = ../outside.glsl\n",
			"C:/Shaders/crt.glslp", chain, error));
	EXPECT_NE(std::string::npos, error.find("relative"));
}

TEST(PostProcessingChainTest, ParsesCrLfRetroArchPresetAndCommaSeparatedParameters)
{
	const std::string preset =
		"shaders = \"1\"\r\n"
		"shader0 = \"shaders/test.glsl\"\r\n"
		"parameters = \"FIRST, SECOND\"\r\n"
		"FIRST = \"0.25\"\r\n"
		"SECOND = \"0.75\"\r\n";
	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslPreset(preset, "C:/Shaders/test.glslp", chain, error)) << error;
	ASSERT_EQ(1u, chain.passes.size());
	EXPECT_EQ(postprocess::ShaderFormat::GLSL, chain.passes[0].format);
	ASSERT_EQ(2u, chain.parameters.size());
	EXPECT_EQ("FIRST", chain.parameters[0].name);
	EXPECT_EQ("SECOND", chain.parameters[1].name);
}

TEST(PostProcessingChainTest, CombinesSelectedShaderFilesAsOrderedLayers)
{
	struct TemporaryDirectory {
		std::filesystem::path path;
		~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
	};
	TemporaryDirectory directory {
		std::filesystem::temp_directory_path() / ("hollycast_shader_stack_"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
	};
	std::filesystem::create_directories(directory.path);
	const std::string sourceShader =
		"#if defined(FRAGMENT)\n"
		"void main() { FragColor = texture(Source, vTexCoord); }\n"
		"#endif\n";
	const std::string previousPassShader =
		"#if defined(FRAGMENT)\n"
		"uniform sampler2D PassPrev0Texture;\n"
		"void main() { FragColor = texture(PassPrev0Texture, vTexCoord); }\n"
		"#endif\n";
	const auto standalone = directory.path / "standalone.glsl";
	const auto presetFirst = directory.path / "preset-first.glsl";
	const auto presetSecond = directory.path / "preset-second.glsl";
	const auto preset = directory.path / "stack.glslp";
	std::ofstream(standalone) << sourceShader;
	std::ofstream(presetFirst) << sourceShader;
	std::ofstream(presetSecond) << previousPassShader;
	std::ofstream(preset) << "shaders = 2\nshader0 = preset-first.glsl\nshader1 = preset-second.glsl\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::loadShaderChain(
			std::vector<std::string> { standalone.string(), preset.string() }, chain, error)) << error;
	EXPECT_TRUE(chain.wantsUnscaledInput);
	ASSERT_EQ(3u, chain.passes.size());
	ASSERT_EQ(3u, chain.sources.size());
	const auto previous = std::find_if(chain.passes[2].inputs.begin(), chain.passes[2].inputs.end(),
			[](const postprocess::ShaderPassInput& input) { return input.bindingName == "PassPrev0Texture"; });
	ASSERT_NE(chain.passes[2].inputs.end(), previous);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, previous->kind);
	EXPECT_EQ(1u, previous->sourcePass);
}

TEST(PostProcessingChainTest, SkipsNestedConditionsInInactiveRetroArchStage)
{
	const std::string shader = R"(
#if defined(VERTEX)
void main() { gl_Position = VertexCoord; }
#elif defined(FRAGMENT)
#define FRAGMENT_ONLY 1
#if FRAGMENT_ONLY
void main() { FragColor = texture(Source, vTexCoord); }
#endif
#endif
)";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_NE(std::string::npos, source.vertex.find("gl_Position"));
	EXPECT_NE(std::string::npos, source.fragment.find("FragColor"));
}

TEST(PostProcessingChainTest, NormalizesCrLfRetroArchGlslInterface)
{
	const std::string shader =
		"#if defined(FRAGMENT)\r\n"
		"uniform vec2 TextureSize;\r\n"
		"#define SourceSize vec4(TextureSize, 1.0 / TextureSize)\r\n"
		"void main() { FragColor = vec4(SourceSize.xy, 0.0, 1.0); }\r\n"
		"#endif\r\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_EQ(std::string::npos, source.fragment.find("#define SourceSize"));
	EXPECT_NE(std::string::npos, source.fragment.find("#define HollycastSourceSize4"));
}

TEST(PostProcessingChainTest, PreprocessorPreservesFunctionMacroSeparatorsAndStripsComments)
{
	const std::string shader =
		"#define SAMPLE(value) vec4(value) // compatibility helper\n"
		"mat4 result = mat4(SAMPLE(1.0), SAMPLE(2.0));\n";
	std::string preprocessed;
	std::string error;
	ASSERT_TRUE(postprocess::preprocessShaderSource(shader, {}, preprocessed, error)) << error;
	EXPECT_NE(std::string::npos, preprocessed.find("mat4(vec4(1.0), vec4(2.0));"));
}

TEST(PostProcessingChainTest, ResolvesRetroArchPreviousPassSamplers)
{
	const std::string shader =
		"#if defined(FRAGMENT)\n"
		"uniform sampler2D PassPrevTexture;\n"
		"uniform sampler2D PassPrev1Texture;\n"
		"uniform sampler2D PassPrev3Texture;\n"
		"uniform sampler2D OrigTexture;\n"
		"void main() { FragColor = texture(PassPrevTexture, vTexCoord)"
				" + texture(PassPrev1Texture, vTexCoord) + texture(PassPrev3Texture, vTexCoord)"
				" + texture(OrigTexture, vTexCoord); }\n"
		"#endif\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	ASSERT_EQ(4u, source.slangResources.size());
	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 2, source, error)) << error;
	ASSERT_EQ(4u, chain.passes[2].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::Source, chain.passes[2].inputs[0].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[2].inputs[1].kind);
	EXPECT_EQ(0u, chain.passes[2].inputs[1].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::Original, chain.passes[2].inputs[2].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::Original, chain.passes[2].inputs[3].kind);
}

TEST(PostProcessingChainTest, LoadsExternalRetroArchGlslCompatibilityPack)
{
	const char *rootValue = std::getenv("HOLLYCAST_RETROARCH_GLSL_TEST_ROOT");
	if (rootValue == nullptr || *rootValue == '\0')
		GTEST_SKIP() << "Set HOLLYCAST_RETROARCH_GLSL_TEST_ROOT to run the upstream compatibility pack.";

	const std::filesystem::path root(rootValue);
	ASSERT_TRUE(std::filesystem::is_directory(root)) << root.string();
	size_t shaderCount = 0;
	size_t presetCount = 0;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file())
			continue;
		const std::string extension = entry.path().extension().string();
		if (extension == ".glsl") {
			std::ifstream stream(entry.path(), std::ios::binary);
			ASSERT_TRUE(stream) << entry.path().string();
			const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			postprocess::ShaderPassSource source;
			std::string error;
			ASSERT_TRUE(postprocess::parseGlslShader(text, source, error))
					<< entry.path().string() << ": " << error;
			EXPECT_EQ(std::string::npos, source.vertex.find("#define SourceSize")) << entry.path().string();
			EXPECT_EQ(std::string::npos, source.fragment.find("#define SourceSize")) << entry.path().string();
			EXPECT_FALSE(std::regex_search(source.vertex, std::regex(R"(\bOutSize\b)", std::regex::icase)))
					<< entry.path().string();
			EXPECT_FALSE(std::regex_search(source.fragment, std::regex(R"(\bOutSize\b)", std::regex::icase)))
					<< entry.path().string();
			std::vector<postprocess::ShaderPassInput> inputs {
				{ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass }
			};
			for (const postprocess::SlangResource& resource : source.slangResources)
				if (resource.name != "Source")
					inputs.push_back({ resource.name, postprocess::ShaderInputKind::Source, {},
							postprocess::InvalidShaderPass });
			std::vector<postprocess::SlangParameter> parameters;
			for (const postprocess::ShaderParameter& parameter : source.parameters) {
				postprocess::SlangParameter typed;
				typed.name = parameter.name;
				typed.type = parameter.type;
				parameters.push_back(std::move(typed));
			}
			std::string hlsl;
			ASSERT_TRUE(postprocess::translateGlslGraphFragmentToHlsl(source, parameters, inputs, 1,
					hlsl, error))
					<< entry.path().string() << ": " << error;
#ifdef _WIN32
			ASSERT_TRUE(compileHlsl(hlsl, "ps_4_0", error)) << entry.path().string() << ": " << error;
#endif
#if defined(USE_VULKAN)
			std::vector<postprocess::ShaderPassInput> vulkanBindings(inputs.begin() + 1, inputs.end());
			std::string vulkanFragment;
			std::string vulkanVertex;
			ASSERT_TRUE(postprocess::buildVulkanShaderSources(source, parameters, vulkanBindings, 1,
					vulkanFragment, vulkanVertex, error)) << entry.path().string() << ": " << error;
			ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eFragment, vulkanFragment, error))
					<< entry.path().string() << ": " << error;
			if (!vulkanVertex.empty())
				ASSERT_TRUE(ShaderCompiler::Validate(vk::ShaderStageFlagBits::eVertex, vulkanVertex, error))
						<< entry.path().string() << ": " << error;
#endif
			if (!source.vertex.empty())
			{
				ASSERT_TRUE(postprocess::translateGlslGraphVertexToHlsl(source.vertex, parameters, inputs, hlsl, error))
						<< entry.path().string() << ": " << error;
#ifdef _WIN32
				ASSERT_TRUE(compileHlsl(hlsl, "vs_4_0", error)) << entry.path().string() << ": " << error;
#endif
			}
			++shaderCount;
		}
		else if (extension == ".glslp") {
			postprocess::ShaderChain chain;
			std::string error;
			ASSERT_TRUE(postprocess::loadShaderChain(entry.path().string(), chain, error))
					<< entry.path().string() << ": " << error;
			for (const postprocess::ShaderPass& pass : chain.passes) {
				postprocess::ShaderPassSource source;
				ASSERT_TRUE(postprocess::loadShaderPassSource(pass, source, error))
						<< pass.sourcePath << ": " << error;
			}
			++presetCount;
		}
	}
	EXPECT_GT(shaderCount, 0u);
	EXPECT_GT(presetCount, 0u);
}

TEST(PostProcessingChainTest, LowersRetroArchGlslInterfaceAndParameters)
{
	const std::string shader = R"(
#version 130
#pragma parameter SCANLINE "Scanline strength" 0.5 0.0 1.0 0.05
#if defined(VERTEX)
in vec4 VertexCoord;
in vec4 TexCoord;
out vec4 TEX0;
uniform mat4 MVPMatrix;
void main() {
	gl_Position = MVPMatrix * VertexCoord;
	TEX0 = TexCoord;
}
#elif defined(FRAGMENT)
uniform sampler2D Texture;
uniform vec2 InputSize;
uniform vec2 TextureSize;
uniform vec2 OutputSize;
uniform int FrameDirection;
uniform int FrameCount;
in vec4 TEX0;
out vec4 FragColor;
void main() {
	FragColor = texture(Texture, TEX0.xy) * (FrameDirection > 0 ? SCANLINE : 1.0);
}
#endif
)";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_TRUE(source.retroArchGlsl);
	EXPECT_NE(std::string::npos, source.vertex.find("gl_Position = VertexCoord"));
	EXPECT_NE(std::string::npos, source.vertex.find("vTexCoord = TexCoord"));
	EXPECT_NE(std::string::npos, source.fragment.find("texture(Source, vTexCoord)"));
	EXPECT_NE(std::string::npos, source.fragment.find("(1) > 0"));
	EXPECT_EQ(std::string::npos, source.fragment.find("TextureSize"));
	ASSERT_EQ(1u, source.parameters.size());
	EXPECT_EQ("SCANLINE", source.parameters[0].name);
	EXPECT_FLOAT_EQ(0.5f, source.parameters[0].value);
	EXPECT_FLOAT_EQ(0.05f, source.parameters[0].step);
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, source.parameters, hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("Source.Sample(SourceSampler, vTexCoord)"));
	ASSERT_TRUE(postprocess::translateGlslVertexToHlsl(source.vertex, {}, hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("output.position = gl_Position"));
}

TEST(PostProcessingChainTest, LowersStandardRetroArchCompatibilityMacros)
{
	const std::string shader = R"(
#version 130
#pragma parameter SCANLINE "Scanline strength" 0.5 0.0 1.0 0.05
#pragma parameter YIQ "YIQ amount" 1.0 0.0 1.0 0.05
#if defined(VERTEX)
#define COMPAT_PRECISION
#define COMPAT_ATTRIBUTE in
#define COMPAT_VARYING out
COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 COLOR;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 COL0;
COMPAT_VARYING vec4 TEX0;
uniform mat4 MVPMatrix;
uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
#define vTexCoord TEX0.xy
#define SourceSize vec4(TextureSize, 1.0 / TextureSize)
#define OutSize vec4(OutputSize, 1.0 / OutputSize)
void main() {
	gl_Position = VertexCoord.x * MVPMatrix[0] + VertexCoord.y * MVPMatrix[1]
			+ VertexCoord.z * MVPMatrix[2] + VertexCoord.w * MVPMatrix[3];
	TEX0.xy = TexCoord.xy;
	COL0 = COLOR;
}
#elif defined(FRAGMENT)
#define COMPAT_PRECISION
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;
#define Source Texture
#define vTexCoord TEX0.xy
#define SourceSize vec4(TextureSize, 1.0 / TextureSize)
#define OutSize vec4(OutputSize, 1.0 / OutputSize)
#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float SCANLINE, YIQ;
#endif
void main() {
	vec4 color = COMPAT_TEXTURE(Source, vTexCoord);
	FragColor = color * (FrameDirection > 0 ? SCANLINE : YIQ)
			* (SourceSize.y > 0.0 && OutSize.y > 0.0 ? 1.0 : 0.0);
}
#endif
)";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseGlslShader(shader, source, error)) << error;
	EXPECT_EQ(std::string::npos, source.vertex.find("COMPAT_ATTRIBUTE"));
	EXPECT_EQ(std::string::npos, source.fragment.find("COMPAT_TEXTURE"));
	EXPECT_EQ(std::string::npos, source.fragment.find("uniform COMPAT_PRECISION"));
	EXPECT_EQ(std::string::npos, source.fragment.find("#define SourceSize"));
	EXPECT_NE(std::string::npos, source.fragment.find("#define HollycastSourceSize4"));
	EXPECT_NE(std::string::npos, source.vertex.find("gl_Position = VertexCoord"));
	EXPECT_EQ(std::string::npos, source.vertex.find("COL0"));
	EXPECT_EQ(std::string::npos, source.vertex.find("COLOR"));
	EXPECT_NE(std::string::npos, source.fragment.find("texture(Source, vTexCoord)"));
	EXPECT_NE(std::string::npos, source.fragment.find("(1) > 0"));
	ASSERT_EQ(2u, source.parameters.size());
	std::string hlsl;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(source.fragment, source.parameters, hlsl, error))
			<< error << '\n' << source.fragment;
	EXPECT_NE(std::string::npos, hlsl.find("Source.Sample(SourceSampler, vTexCoord)"));
	ASSERT_TRUE(postprocess::translateGlslVertexToHlsl(source.vertex, {}, hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("output.position = gl_Position"));
}

TEST(PostProcessingChainTest, ParsesNamedPresetResourcesAndTargetRequests)
{
	const std::string preset =
		"shaders = 2\n"
		"shader0 = first.slang\n"
		"alias0 = First\n"
		"float_framebuffer0 = true\n"
		"shader1 = final.slang\n"
		"alias1 = Final\n"
		"srgb_framebuffer1 = true\n"
		"feedback_pass = 1\n"
		"textures = LUT\n"
		"LUT = assets/lut.png\n"
		"LUT_linear = true\n"
		"LUT_mipmap = true\n"
		"LUT_wrap_mode = repeat\n";

	postprocess::ShaderChain chain;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangPreset(preset, "C:/Shaders/crt.slangp", chain, error)) << error;
	EXPECT_EQ(postprocess::ShaderTargetFormat::Float, chain.passes[0].targetFormat);
	EXPECT_EQ(postprocess::ShaderTargetFormat::Srgb, chain.passes[1].targetFormat);
	EXPECT_TRUE(chain.passes[1].feedbackTarget);
	EXPECT_EQ(1u, chain.feedbackPass);
	ASSERT_EQ(1u, chain.textures.size());
	EXPECT_EQ("LUT", chain.textures[0].name);
	std::string texturePath = chain.textures[0].path;
	std::replace(texturePath.begin(), texturePath.end(), '\\', '/');
	EXPECT_EQ("C:/Shaders/assets/lut.png", texturePath);
	EXPECT_TRUE(chain.textures[0].linearFilter);
	EXPECT_TRUE(chain.textures[0].useMipmaps);
	EXPECT_EQ(postprocess::ShaderWrapMode::Repeat, chain.textures[0].wrapMode);
}

TEST(PostProcessingChainTest, ComputesOnlyRequiredOutputMipChains)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	chain.passes[0].outputs.resize(2);
	chain.passes[0].outputs[0].useMipmaps = true;
	chain.passes[2].useMipmaps = true;
	postprocess::ShaderPassInput alias;
	alias.kind = postprocess::ShaderInputKind::PassOutput;
	alias.sourcePass = 0;
	alias.sourceOutput = 1;
	chain.passes[2].inputs.push_back(alias);

	const std::vector<std::vector<bool>> requirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	ASSERT_EQ(3u, requirements.size());
	ASSERT_EQ(2u, requirements[0].size());
	EXPECT_TRUE(requirements[0][0]);
	EXPECT_TRUE(requirements[0][1]);
	ASSERT_EQ(1u, requirements[1].size());
	EXPECT_TRUE(requirements[1][0]);
	ASSERT_EQ(1u, requirements[2].size());
	EXPECT_FALSE(requirements[2][0]);
}

TEST(PostProcessingChainTest, RejectsUnsafeOrAmbiguousPresetResources)
{
	postprocess::ShaderChain chain;
	std::string error;
	EXPECT_FALSE(postprocess::parseSlangPreset("shaders = 1\nshader0 = ../outside.slang\n",
			"C:/Shaders/crt.slangp", chain, error));
	EXPECT_NE(std::string::npos, error.find("relative"));
	EXPECT_FALSE(postprocess::parseSlangPreset(
			"shaders = 1\nshader0 = pass.slang\nalias0 = Pass\nfloat_framebuffer0 = true\nsrgb_framebuffer0 = true\n",
			"C:/Shaders/crt.slangp", chain, error));
	EXPECT_NE(std::string::npos, error.find("both float and sRGB"));
}

TEST(PostProcessingChainTest, SplitsSlangStagesAndReflectsParameters)
{
	const std::string source =
		"#version 450\n"
		"#pragma format R16G16B16A16_SFLOAT\n"
		"#pragma parameter BLOOM \"Bloom strength\" 0.5 0.0 1.0 0.05\n"
		"#pragma stage vertex\n"
		"void vertexMain() {}\n"
		"#pragma stage fragment\n"
		"void fragmentMain() {}\n";

	postprocess::SlangShaderSource shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangShader(source, shader, error)) << error;
	EXPECT_NE(std::string::npos, shader.vertex.find("vertexMain"));
	EXPECT_EQ(std::string::npos, shader.vertex.find("fragmentMain"));
	EXPECT_NE(std::string::npos, shader.fragment.find("fragmentMain"));
	ASSERT_EQ(1u, shader.parameters.size());
	EXPECT_EQ("BLOOM", shader.parameters[0].name);
	EXPECT_EQ("Bloom strength", shader.parameters[0].label);
	EXPECT_FLOAT_EQ(0.05f, shader.parameters[0].step);

	postprocess::SlangShaderIR metadata;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, metadata, error)) << error;
	EXPECT_EQ("R16G16B16A16_SFLOAT", metadata.format);
}

TEST(PostProcessingChainTest, ResolvesSlangResourceGraphBindings)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(2);
	chain.passes[0].alias = "First";
	chain.passes[1].alias = "Final";
	chain.feedbackPass = 1;
	chain.passes[1].feedbackTarget = true;
	chain.textures.push_back({ "LUT", "C:/Shaders/lut.png" });

	postprocess::ShaderPassSource source;
	source.slangResources = {
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::Source, "Source" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::Original, "Original" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::PassAlias, "FirstTexture", "First" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::History, "FirstHistory", "First" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::Feedback, "FinalFeedback", "Final" },
		{ postprocess::SlangResourceType::Sampler2D, postprocess::SlangResourceConvention::PassAlias, "LUT", "LUT" },
	};

	std::string error;
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 1, source, error)) << error;
	ASSERT_EQ(6u, chain.passes[1].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[1].inputs[2].kind);
	EXPECT_EQ(0u, chain.passes[1].inputs[2].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::History, chain.passes[1].inputs[3].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, chain.passes[1].inputs[4].kind);
	EXPECT_EQ(1u, chain.passes[1].inputs[4].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::LookupTexture, chain.passes[1].inputs[5].kind);

	EXPECT_FALSE(postprocess::resolveShaderPassInputs(chain, 0, source, error));
	EXPECT_NE(std::string::npos, error.find("earlier pass alias"));
}

TEST(PostProcessingChainTest, ClassifiesSlangTextureSuffixConventions)
{
	const std::string source =
		"#version 450\n"
		"uniform sampler2D SourceTexture;\n"
		"uniform sampler2D OriginalTexture;\n"
		"uniform sampler2D FirstHistoryTexture;\n"
		"uniform sampler2D FinalFeedbackTexture;\n"
		"uniform sampler2D FirstTexture;\n"
		"void main() {}\n";

	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(source, shader, error)) << error;
	ASSERT_EQ(5u, shader.resources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, shader.resources[0].convention);
	EXPECT_EQ(postprocess::SlangResourceConvention::Original, shader.resources[1].convention);
	EXPECT_EQ("First", shader.resources[2].alias);
	EXPECT_EQ(postprocess::SlangResourceConvention::History, shader.resources[2].convention);
	EXPECT_EQ("Final", shader.resources[3].alias);
	EXPECT_EQ(postprocess::SlangResourceConvention::Feedback, shader.resources[3].convention);
	EXPECT_EQ("First", shader.resources[4].alias);
}

TEST(PostProcessingChainTest, ResolvesStandardSlangIndexedResources)
{
	const std::string shaderText =
		"#version 450\n"
		"layout(binding = 0) uniform sampler2D OriginalHistory0;\n"
		"layout(binding = 1) uniform sampler2D OriginalHistory2;\n"
		"layout(binding = 2) uniform sampler2D PassOutput0;\n"
		"layout(binding = 3) uniform sampler2D PassFeedback1;\n"
		"layout(binding = 4) uniform sampler2D User0Sampler;\n"
		"#pragma stage fragment\nvoid main() {}\n";
	postprocess::SlangShaderIR shader;
	std::string error;
	ASSERT_TRUE(postprocess::parseSlangFrontend(shaderText, shader, error)) << error;

	postprocess::ShaderChain chain;
	chain.passes.resize(3);
	chain.textures.push_back({ "LUT", "C:/Shaders/lut.png" });
	postprocess::ShaderPassSource source;
	source.slangResources = shader.resources;
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 2, source, error)) << error;
	ASSERT_EQ(5u, chain.passes[2].inputs.size());
	EXPECT_EQ(postprocess::ShaderInputKind::Original, chain.passes[2].inputs[0].kind);
	EXPECT_EQ(postprocess::ShaderInputKind::History, chain.passes[2].inputs[1].kind);
	EXPECT_EQ(2u, chain.passes[2].inputs[1].historyOffset);
	EXPECT_EQ(postprocess::ShaderInputKind::PassOutput, chain.passes[2].inputs[2].kind);
	EXPECT_EQ(0u, chain.passes[2].inputs[2].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::Feedback, chain.passes[2].inputs[3].kind);
	EXPECT_EQ(1u, chain.passes[2].inputs[3].sourcePass);
	EXPECT_EQ(postprocess::ShaderInputKind::LookupTexture, chain.passes[2].inputs[4].kind);
	EXPECT_EQ("LUT", chain.passes[2].inputs[4].resourceName);
}

TEST(PostProcessingChainTest, RejectsExcessiveHistoryRetention)
{
	postprocess::ShaderChain chain;
	chain.passes.resize(1);
	postprocess::ShaderPassSource source;
	postprocess::SlangResource history;
	history.type = postprocess::SlangResourceType::Sampler2D;
	history.convention = postprocess::SlangResourceConvention::History;
	history.name = "OriginalHistory32";
	history.alias = "Original";
	history.historyOffset = 32;
	source.slangResources.push_back(std::move(history));
	std::string error;

	EXPECT_FALSE(postprocess::resolveShaderPassInputs(chain, 0, source, error));
	EXPECT_NE(std::string::npos, error.find("at most 32 frames"));
}

TEST(PostProcessingChainTest, ResolvesPassTargetsFromTheSharedFrameContract)
{
	postprocess::ShaderPass pass;
	pass.scaleTypeX = postprocess::ScaleType::Viewport;
	pass.scaleTypeY = postprocess::ScaleType::Source;
	pass.scaleX = 0.5f;
	pass.scaleY = 2.f;
	postprocess::ShaderFrameInfo frame { 320, 240, 1280, 720, 42 };
	int width = 0;
	int height = 0;
	std::string error;

	EXPECT_TRUE(postprocess::resolveTargetSize(pass, frame, width, height, error));
	EXPECT_EQ(640, width);
	EXPECT_EQ(480, height);

	pass.scaleTypeX = postprocess::ScaleType::Absolute;
	pass.scaleX = 0.f;
	EXPECT_FALSE(postprocess::resolveTargetSize(pass, frame, width, height, error));
}

TEST(PostProcessingChainTest, ComposesOnlyNormalDisplayInputs)
{
	postprocess::ShaderFrameInfo frame { 640, 480, 1280, 960, 42, false };
	EXPECT_TRUE(postprocess::shouldComposeDisplayInput(frame, false));
	EXPECT_FALSE(postprocess::shouldComposeDisplayInput(frame, true));
	frame.sourceWidth = frame.viewportWidth;
	frame.sourceHeight = frame.viewportHeight;
	EXPECT_FALSE(postprocess::shouldComposeDisplayInput(frame, false));
	frame.rotationPending = true;
	EXPECT_TRUE(postprocess::shouldComposeDisplayInput(frame, false));
}

TEST(PostProcessingChainTest, TranslatesPortableGlslFragmentBodiesToHlsl)
{
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { gl_FragColor = mix(texture(Source, vTexCoord), vec4(1.0), 0.25); }", hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("float4 main(PixelInput input)"));
	EXPECT_NE(std::string::npos, hlsl.find("Source.Sample(SourceSampler, vTexCoord)"));
	EXPECT_NE(std::string::npos, hlsl.find("lerp("));
}

TEST(PostProcessingChainTest, TranslatesPortableScanlineShaderToHlsl)
{
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { vec4 color = texture(Source, vTexCoord); float scanlineFactor = mix(0.55, 1.0, mod(floor(vTexCoord.y * OutputSize.y), 2.0)); gl_FragColor = vec4(color.rgb * scanlineFactor, color.a); }",
			hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("GlslMod(floor(vTexCoord.y * OutputSize.y), 2.0)"));
	EXPECT_NE(std::string::npos, hlsl.find("float4 main(PixelInput input) : SV_TARGET"));
}

TEST(PostProcessingChainTest, TranslatesGlslScalarVectorConstructorsToHlslCasts)
{
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { float luminance = 0.5; gl_FragColor = vec4(vec3(luminance), 1.0); }",
			hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("(float3)(luminance)"));
	EXPECT_NE(std::string::npos, hlsl.find("float4((float3)(luminance), 1.0)"));
}

TEST(PostProcessingChainTest, PacksDx11ParametersContiguously)
{
	std::vector<postprocess::SlangParameter> parameters(3);
	parameters[0].name = "RED";
	parameters[1].name = "GREEN";
	parameters[2].name = "BLUE";
	std::string hlsl;
	std::string error;
	ASSERT_TRUE(postprocess::translateGlslFragmentToHlsl(
			"void main() { gl_FragColor = texture(Source, vTexCoord) * vec4(RED, GREEN, BLUE, 1.0); }",
			parameters, hlsl, error)) << error;
	EXPECT_NE(std::string::npos, hlsl.find("float4 Parameters[6]"));
	EXPECT_NE(std::string::npos, hlsl.find("static float RED = Parameters[0].x;"));
	EXPECT_NE(std::string::npos, hlsl.find("static float GREEN = Parameters[0].y;"));
	EXPECT_NE(std::string::npos, hlsl.find("static float BLUE = Parameters[0].z;"));
}

TEST(PostProcessingChainTest, RejectsUnbindableGlslResourcesForHlslTranslation)
{
	std::string hlsl;
	std::string error;
	EXPECT_FALSE(postprocess::translateGlslFragmentToHlsl(
			"uniform sampler2D Other; void main() { gl_FragColor = texture(Other, vec2(0.0)); }", hlsl, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, LowersReShadeFxStructsHelpersResourcesAndControlFlow)
{
	const std::string effect =
		"#define ENABLE_BLOOM 1\n"
		"uniform float BLOOM < ui_label = \"Bloom\"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.05; > = 0.75;\n"
		"uniform float4 Tint = float4(1.0, 0.9, 0.8, 1.0);\n"
		"texture2D SourceTex < source = \"color\"; > { Width = BUFFER_WIDTH; };\n"
		"sampler2D SourceSampler { Texture = SourceTex; Filter = MIN_MAG_MIP_LINEAR; };\n"
		"struct PixelData { float2 texcoord : TEXCOORD0; };\n"
		"float4 ApplyBloom(float4 color) { for (int i = 0; i < 1; ++i) color = lerp(color, Tint, BLOOM); return saturate(color); }\n"
		"technique Main { pass { PixelShader = MainPS; } }\n"
		"float4 MainPS(PixelData input) : SV_Target { return ApplyBloom(tex2D(SourceSampler, input.texcoord)); }\n";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	EXPECT_NE(std::string::npos, source.fragment.find("void main"));
	EXPECT_NE(std::string::npos, source.fragment.find("texture(SourceSampler, input.texcoord)"));
	EXPECT_NE(std::string::npos, source.fragment.find("for (int i = 0"));
	EXPECT_NE(std::string::npos, source.fragment.find("ApplyBloom"));
	ASSERT_EQ(2u, source.parameters.size());
	EXPECT_EQ("BLOOM", source.parameters[0].name);
	EXPECT_EQ("Tint", source.parameters[1].name);
	ASSERT_EQ(2u, source.slangParameters.size());
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ(postprocess::SlangResourceConvention::Source, source.slangResources[0].convention);
}

TEST(PostProcessingChainTest, RejectsNonPortableReShadeFxBeforeActivation)
{
	postprocess::ShaderPassSource source;
	std::string error;
	EXPECT_FALSE(postprocess::parseReShadeFx(
			"technique Main { pass { PixelShader = MainPS; } }\n"
			"float4 MainPS() : SV_Target { return tex2D(OtherSampler, float2(0.0, 0.0)); }\n", source, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, ReflectsReShadeChoiceParameters)
{
	const std::string effect =
		"uniform int Mode < ui_label = \"Quality\"; ui_type = \"combo\"; ui_items = \"Low\\0Medium\\0High\\0\"; > = 1;\n"
		"float4 MainPS() : SV_Target { return float4(Mode, 0.0, 0.0, 1.0); }\n"
		"technique Main { pass { PixelShader = MainPS; } }\n";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	ASSERT_EQ(1u, source.parameters.size());
	const postprocess::ShaderParameter& parameter = source.parameters[0];
	EXPECT_EQ(postprocess::SlangParameterType::Int, parameter.type);
	EXPECT_EQ(0.f, parameter.minimum);
	EXPECT_EQ(2.f, parameter.maximum);
	EXPECT_EQ(1.f, parameter.step);
	EXPECT_EQ((std::vector<std::string> { "Low", "Medium", "High" }), parameter.choiceLabels);
}

TEST(PostProcessingChainTest, SnapsPersistedParametersToDeclaredSteps)
{
	postprocess::ShaderChain chain;
	postprocess::ShaderParameter parameter;
	parameter.name = "MODE";
	parameter.label = "Mode";
	parameter.minimum = parameter.minimumValues[0] = 0.f;
	parameter.maximum = parameter.maximumValues[0] = 1.f;
	parameter.step = parameter.stepValues[0] = 1.f;
	chain.parameters.push_back(parameter);

	postprocess::applyShaderChainParameterValues(chain, "MODE=0.6");
	EXPECT_EQ(1.f, chain.parameters[0].value);
}

TEST(PostProcessingChainTest, ParsesReShadeFxIntoBackendNeutralSyntaxIr)
{
	const std::string effect =
		"// Lexer must ignore include-looking comments. #include \"other.fx\"\n"
		"#define ENABLE_CRT 1\n"
		"uniform float4 Tint < ui_label = \"Tint Color\"; > = float4(1.0, 0.9, 0.8, 1.0);\n"
		"texture2D SourceTex < string source = \"color\"; > { Width = BUFFER_WIDTH; };\n"
		"sampler2D SourceSampler { Texture = SourceTex; };\n"
		"struct PixelData { float2 texcoord : TEXCOORD0; };\n"
		"float4 MainPS(PixelData input) : SV_Target { return tex2D(SourceSampler, input.texcoord) * Tint; }\n"
		"technique Main { pass First { VertexShader = FullscreenVS; PixelShader = MainPS; } }\n";

	postprocess::ReShadeEffect parsed;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeEffect(effect, parsed, error)) << error;
	ASSERT_EQ(1u, parsed.preprocessorDirectives.size());
	ASSERT_EQ(1u, parsed.uniforms.size());
	EXPECT_EQ(postprocess::ReShadeValueType::Float4, parsed.uniforms[0].valueType);
	EXPECT_EQ("Tint Color", parsed.uniforms[0].annotations[0].value);
	ASSERT_EQ(1u, parsed.textures.size());
	ASSERT_EQ(1u, parsed.samplers.size());
	ASSERT_EQ(1u, parsed.structs.size());
	ASSERT_EQ(1u, parsed.functions.size());
	EXPECT_NE(std::string::npos, parsed.functions[0].body.find("tex2D"));
	ASSERT_TRUE(parsed.selectedTechnique.has_value());
	ASSERT_TRUE(parsed.selectedPass.has_value());
	EXPECT_EQ("MainPS", parsed.techniques[*parsed.selectedTechnique].passes[*parsed.selectedPass].pixelShader);
}

TEST(PostProcessingChainTest, RejectsReShadeFxIncludesBeforeParsing)
{
	postprocess::ReShadeEffect parsed;
	std::string error;
	EXPECT_FALSE(postprocess::parseReShadeEffect("# include \\\"outside.fx\\\"\n", parsed, error));
	EXPECT_FALSE(error.empty());
}

TEST(PostProcessingChainTest, AppliesReShadeFxConditionalPreprocessingBeforeParsing)
{
	const std::string effect =
		"#define ENABLE_EFFECT 1\n"
		"#if ENABLE_EFFECT\n"
		"float4 EnabledPS() : SV_Target { return float4(1.0); }\n"
		"#else\n"
		"float4 DisabledPS() : SV_Target { return float4(0.0); }\n"
		"#endif\n"
		"technique Main { pass { PixelShader = EnabledPS; } }\n";
	postprocess::ReShadeEffect parsed;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeEffect(effect, parsed, error)) << error;
	ASSERT_EQ(1u, parsed.functions.size());
	EXPECT_EQ("EnabledPS", parsed.functions[0].name);
}

TEST(PostProcessingChainTest, AppliesReShadeFxCompoundPreprocessorExpressionsAndMacros)
{
	const std::string effect =
		"#define QUALITY 2\n"
		"#define ENABLED 1\n"
		"#define COLOR(value) float4(value)\n"
		"#if defined(ENABLED) && QUALITY >= 2 && !defined(DISABLED)\n"
		"#define RETURN_COLOR COLOR(1.0)\n"
		"#else\n"
		"#define RETURN_COLOR float4(0.0)\n"
		"#endif\n"
		"float4 MainPS() : SV_Target { return RETURN_COLOR; }\n"
		"technique Main { pass { PixelShader = MainPS; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::parseReShadeFx(effect, source, error)) << error;
	EXPECT_NE(std::string::npos, source.fragment.find("vec4(1.0)")) << source.fragment;
}

TEST(PostProcessingChainTest, SerializesRichShaderParameterValues)
{
	postprocess::ShaderParameter parameter;
	parameter.name = "Tint";
	parameter.label = "Tint";
	parameter.componentCount = 3;
	parameter.type = postprocess::SlangParameterType::Float3;
	parameter.values = { 0.25f, 0.5f, 0.75f };
	parameter.minimumValues = { 0.f, 0.f, 0.f };
	parameter.maximumValues = { 1.f, 1.f, 1.f };
	parameter.stepValues = { 0.01f, 0.01f, 0.01f };
	parameter.value = parameter.values[0];
	std::vector<postprocess::ShaderParameter> parameters { parameter };
	postprocess::ShaderChain chain;
	chain.parameters = parameters;
	postprocess::applyShaderChainParameterValues(chain,
			postprocess::serializeShaderChainParameterValues(parameters));
	ASSERT_EQ(1u, chain.parameters.size());
	EXPECT_FLOAT_EQ(0.25f, chain.parameters[0].values[0]);
	EXPECT_FLOAT_EQ(0.5f, chain.parameters[0].values[1]);
	EXPECT_FLOAT_EQ(0.75f, chain.parameters[0].values[2]);
}

TEST(PostProcessingChainTest, LowersReShadeFxResourcesForBackendBinding)
{
	postprocess::ShaderPassSource source;
	std::string error;
	EXPECT_TRUE(postprocess::parseReShadeFx(
			"texture2D SourceTex < source = \"color\"; > { Width = BUFFER_WIDTH; };\n"
			"sampler2D SourceSampler { Texture = SourceTex; };\n"
			"technique Main { pass { PixelShader = MainPS; } }\n"
			"float4 MainPS() : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n", source, error));
	EXPECT_TRUE(error.empty());
	ASSERT_EQ(1u, source.slangResources.size());
	EXPECT_EQ("SourceSampler", source.slangResources[0].name);
}

TEST(PostProcessingChainTest, LowersSelectedReShadeTechniquePassAndVertexStage)
{
	const std::string effect =
		"struct VertexData { float4 position : SV_Position; float2 texcoord : TEXCOORD0; };\n"
		"VertexData MainVS(float4 position : POSITION, float2 texcoord : TEXCOORD0) { VertexData output; output.position = position; output.texcoord = texcoord; return output; }\n"
		"float4 FirstPS() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }\n"
		"float4 SecondPS() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n"
		"technique Main { pass First { VertexShader = MainVS; PixelShader = FirstPS; } pass Second { PixelShader = SecondPS; } }\n";

	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	EXPECT_NE(std::string::npos, source.vertex.find("fxVertexResult"));
	EXPECT_NE(std::string::npos, source.fragment.find("FirstPS"));
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 1, source, error)) << error;
	EXPECT_TRUE(source.vertex.empty());
	EXPECT_NE(std::string::npos, source.fragment.find("SecondPS"));
}

TEST(PostProcessingChainTest, ReflectsAndRoutesReShadeMultiRenderTargets)
{
	const std::string effect =
		"struct Outputs { float4 color : SV_Target0; float4 bloom : SV_Target1; };\n"
		"Outputs MainPS() { Outputs output; output.color = float4(1.0); output.bloom = float4(0.5); return output; }\n"
		"technique Main { pass { PixelShader = MainPS; RenderTarget0 = ColorTarget; RenderTarget1 = BloomTarget; } }\n";
	postprocess::ShaderPassSource source;
	std::string error;
	ASSERT_TRUE(postprocess::compileReShadeFxPass(effect, 0, 0, source, error)) << error;
	EXPECT_EQ(2u, source.outputCount);
	EXPECT_NE(std::string::npos, source.fragment.find("FxOutput1"));

	postprocess::ShaderChain chain;
	chain.passes.resize(2);
	chain.passes[0].alias = "ColorTarget";
	chain.passes[0].outputs.push_back({ "ColorTarget" });
	chain.passes[0].outputs.push_back({ "BloomTarget" });
	postprocess::ShaderPassSource consumer;
	postprocess::SlangResource bloomSampler;
	bloomSampler.type = postprocess::SlangResourceType::Sampler2D;
	bloomSampler.convention = postprocess::SlangResourceConvention::PassAlias;
	bloomSampler.name = "BloomSampler";
	bloomSampler.alias = "BloomTarget";
	consumer.slangResources.push_back(std::move(bloomSampler));
	ASSERT_TRUE(postprocess::resolveShaderPassInputs(chain, 1, consumer, error)) << error;
	ASSERT_EQ(1u, chain.passes[1].inputs.size());
	EXPECT_EQ(0u, chain.passes[1].inputs[0].sourcePass);
	EXPECT_EQ(1u, chain.passes[1].inputs[0].sourceOutput);
}
