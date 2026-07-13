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

#include <gtest/gtest.h>

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
	EXPECT_EQ("C:/Shaders/assets/lut.png", chain.textures[0].path);
	EXPECT_TRUE(chain.textures[0].linearFilter);
	EXPECT_TRUE(chain.textures[0].useMipmaps);
	EXPECT_EQ(postprocess::ShaderWrapMode::Repeat, chain.textures[0].wrapMode);
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
	ASSERT_EQ(1u, source.parameters.size());
	EXPECT_EQ("BLOOM", source.parameters[0].name);
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

TEST(PostProcessingChainTest, ParsesReShadeFxIntoBackendNeutralSyntaxIr)
{
	const std::string effect =
		"// Lexer must ignore include-looking comments. #include \\\"other.fx\\\"\n"
		"#define ENABLE_CRT 1\n"
		"uniform float4 Tint < ui_label = \\\"Tint Color\\\"; > = float4(1.0, 0.9, 0.8, 1.0);\n"
		"texture2D SourceTex < string source = \\\"color\\\"; > { Width = BUFFER_WIDTH; };\n"
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
	EXPECT_NE(std::string::npos, source.fragment.find("float4(1.0)"));
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
