/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "gl_shader_chain.h"

#include "glcache.h"
#include "cfg/option.h"
#include "log/LogManager.h"
#include "oslib/storage.h"
#include "stbi.h"
#include <algorithm>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr char FullscreenVertexShader[] = R"(
in highp vec3 in_pos;
in mediump vec2 in_uv;
out mediump vec2 vTexCoord;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
	vTexCoord = in_uv;
}
)";

constexpr char RotatedFullscreenVertexShader[] = R"(
in highp vec3 in_pos;
in mediump vec2 in_uv;
out mediump vec2 vTexCoord;

void main()
{
	gl_Position = vec4(-in_pos.y, in_pos.x, in_pos.z, 1.0);
	vTexCoord = in_uv;
}
)";

const char *postProcessGlslVersion()
{
	// The normal renderer keeps a GLSL 1.30 baseline for older OpenGL drivers.
	// Shader packages are allowed to use modern local expressions, so use the
	// full language version when the active context is already OpenGL 4.3+.
	if (!gl.is_gles && (gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 3)))
		return "#version 430";
	if (!gl.is_gles && (gl.gl_major > 3 || (gl.gl_major == 3 && gl.gl_minor >= 3)))
		return "#version 330";
	return gl.glsl_version_header;
}

class PostProcessSource final : public ShaderSource
{
public:
	PostProcessSource() : ShaderSource(postProcessGlslVersion())
	{
		addConstant("TARGET_GL", gl.gl_version);
		addSource(ShaderCompatSource);
	}
};

void removeVersionDirective(std::string& source)
{
	const size_t lineEnd = source.find_first_of("\r\n");
	const std::string firstLine = source.substr(0, lineEnd);
	if (firstLine.rfind("#version", 0) != 0)
		return;
	source.erase(0, lineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", lineEnd));
}

const char *glslType(postprocess::SlangParameterType type)
{
	switch (type) {
	case postprocess::SlangParameterType::Float: return "float";
	case postprocess::SlangParameterType::Int: return "int";
	case postprocess::SlangParameterType::Bool: return "bool";
	case postprocess::SlangParameterType::Float2: return "vec2";
	case postprocess::SlangParameterType::Float3: return "vec3";
	case postprocess::SlangParameterType::Float4: return "vec4";
	case postprocess::SlangParameterType::Float2x2: return "mat2";
	case postprocess::SlangParameterType::Float3x3: return "mat3";
	case postprocess::SlangParameterType::Float4x4: return "mat4";
	}
	return "float";
}

bool hasUniformDeclaration(const std::string& source, const std::string& name)
{
	const std::regex declaration("\\buniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?[A-Za-z_][A-Za-z0-9_]*\\s+" + name + "\\b");
	return std::regex_search(source, declaration);
}

bool hasInputDeclaration(const std::string& source, const std::string& name)
{
	const std::regex declaration("\\b(?:in|varying)\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?[A-Za-z_][A-Za-z0-9_]*\\s+" + name + "\\b");
	return std::regex_search(source, declaration);
}

std::string fragmentPreamble(const std::vector<postprocess::ShaderParameter>& parameters,
		const std::vector<postprocess::SlangParameter>& slangParameters,
		const std::vector<postprocess::SlangResource>& resources, const std::string& source, uint32_t outputCount)
{
	std::string preamble;
	if (outputCount == 1)
		preamble = "#define FxOutput0 gl_FragColor\n";
	else
		for (uint32_t output = 0; output < outputCount; ++output)
			preamble += "layout(location = " + std::to_string(output) + ") out vec4 FxOutput" + std::to_string(output) + ";\n";
	if (!hasInputDeclaration(source, "vTexCoord"))
		preamble += "in mediump vec2 vTexCoord;\n";
	bool hasSource = false;
	for (const postprocess::SlangResource& resource : resources) {
		preamble += "uniform sampler2D " + resource.name + ";\n";
		hasSource = hasSource || resource.name == "Source";
	}
	if (!hasSource && !hasUniformDeclaration(source, "Source")) preamble += "uniform sampler2D Source;\n";
	if (!hasUniformDeclaration(source, "SourceSize")) preamble += "uniform mediump vec2 SourceSize;\n";
	if (!hasUniformDeclaration(source, "OriginalSize")) preamble += "uniform mediump vec2 OriginalSize;\n";
	if (!hasUniformDeclaration(source, "OutputSize")) preamble += "uniform mediump vec2 OutputSize;\n";
	if (!hasUniformDeclaration(source, "FrameCount")) preamble += "uniform int FrameCount;\n";
	if (!hasUniformDeclaration(source, "Rotation")) preamble += "uniform int Rotation;\n";
	std::unordered_set<std::string> emittedParameters;
	for (const postprocess::ShaderParameter& parameter : parameters)
		if (!hasUniformDeclaration(source, parameter.name) && emittedParameters.emplace(parameter.name).second)
			preamble += "uniform " + std::string(glslType(parameter.type)) + " " + parameter.name + ";\n";
	for (const postprocess::SlangParameter& parameter : slangParameters)
		if (!hasUniformDeclaration(source, parameter.name) && emittedParameters.emplace(parameter.name).second)
			preamble += "uniform " + std::string(glslType(parameter.type)) + " " + parameter.name + ";\n";
	return preamble;
}

void removeResourceDeclarations(std::string& source)
{
	static const std::regex resource(R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:sampler|texture|image)(?:2DArray|1D|2D|3D|Cube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[\s*[1-9][0-9]*\s*\])?\s*;\s*$)");
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);)
		if (!std::regex_match(line, resource))
			cleaned += line + '\n';
	source = std::move(cleaned);
}

std::string vertexResourcePreamble(const std::vector<postprocess::SlangResource>& resources)
{
	// Portable custom stages have their interface declarations stripped before
	// backend compilation, so recreate the fixed fullscreen varying here.
	std::string preamble = "out mediump vec2 vTexCoord;\n";
	for (const postprocess::SlangResource& resource : resources)
		preamble += "uniform sampler2D " + resource.name + ";\n";
	return preamble;
}

bool isTexture2D(postprocess::SlangResourceType type)
{
	return type == postprocess::SlangResourceType::Sampler2D || type == postprocess::SlangResourceType::Texture2D;
}

GLint glWrapMode(postprocess::ShaderWrapMode mode)
{
	switch (mode) {
	case postprocess::ShaderWrapMode::Repeat: return GL_REPEAT;
	case postprocess::ShaderWrapMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
	case postprocess::ShaderWrapMode::ClampToBorder:
		// Desktop GL guarantees transparent border sampling. GLES has no portable
		// border-clamp state, so retain edge clamp there.
#ifndef GLES
		return GL_CLAMP_TO_BORDER;
#else
		return GL_CLAMP_TO_EDGE;
#endif
	case postprocess::ShaderWrapMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
	}
	return GL_CLAMP_TO_EDGE;
}

GLenum glBlendFactor(postprocess::ShaderBlendFactor factor)
{
	switch (factor) {
	case postprocess::ShaderBlendFactor::Zero: return GL_ZERO;
	case postprocess::ShaderBlendFactor::One: return GL_ONE;
	case postprocess::ShaderBlendFactor::SourceColor: return GL_SRC_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusSourceColor: return GL_ONE_MINUS_SRC_COLOR;
	case postprocess::ShaderBlendFactor::DestinationColor: return GL_DST_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusDestinationColor: return GL_ONE_MINUS_DST_COLOR;
	case postprocess::ShaderBlendFactor::SourceAlpha: return GL_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusSourceAlpha: return GL_ONE_MINUS_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::DestinationAlpha: return GL_DST_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusDestinationAlpha: return GL_ONE_MINUS_DST_ALPHA;
	}
	return GL_ONE;
}

GLenum glBlendOperation(postprocess::ShaderBlendOperation operation)
{
	switch (operation) {
	case postprocess::ShaderBlendOperation::Add: return GL_FUNC_ADD;
	case postprocess::ShaderBlendOperation::Subtract: return GL_FUNC_SUBTRACT;
	case postprocess::ShaderBlendOperation::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
	case postprocess::ShaderBlendOperation::Minimum: return GL_MIN;
	case postprocess::ShaderBlendOperation::Maximum: return GL_MAX;
	}
	return GL_FUNC_ADD;
}

bool glTargetFormat(const postprocess::ShaderPass& pass, GLenum& internalFormat, GLenum& pixelFormat,
		GLenum& pixelType, std::string& error)
{
	if (!pass.targetFormatName.empty() && pass.targetFormatName != "R8G8B8A8_UNORM"
			&& pass.targetFormatName != "R8G8B8A8_SRGB" && pass.targetFormatName != "R16G16B16A16_SFLOAT"
			&& pass.targetFormatName != "R16G16B16A16_UNORM" && pass.targetFormatName != "R32G32B32A32_SFLOAT") {
		error = "OpenGL does not support post-processing target format '" + pass.targetFormatName + "'.";
		return false;
	}
	const bool wantsFloat = pass.targetFormat == postprocess::ShaderTargetFormat::Float
			|| pass.targetFormatName == "R16G16B16A16_SFLOAT" || pass.targetFormatName == "R32G32B32A32_SFLOAT";
	const bool wantsSrgb = pass.targetFormat == postprocess::ShaderTargetFormat::Srgb
			|| pass.targetFormatName == "R8G8B8A8_SRGB";
	if (pass.targetFormatName == "R16G16B16A16_UNORM") {
		if (gl.gl_major < 3) {
			error = "OpenGL post-processing 16-bit targets require OpenGL or GLES 3.0.";
			return false;
		}
		internalFormat = GL_RGBA16;
		pixelFormat = GL_RGBA;
		pixelType = GL_UNSIGNED_SHORT;
		return true;
	}
	if (wantsFloat || wantsSrgb) {
		if (gl.gl_major < 3) {
			error = "OpenGL post-processing float and sRGB targets require OpenGL or GLES 3.0.";
			return false;
		}
		if (wantsFloat) {
			internalFormat = pass.targetFormatName == "R32G32B32A32_SFLOAT" ? GL_RGBA32F : GL_RGBA16F;
			pixelFormat = GL_RGBA;
			pixelType = pass.targetFormatName == "R32G32B32A32_SFLOAT" ? GL_FLOAT : GL_HALF_FLOAT;
			return true;
		}
		internalFormat = GL_SRGB8_ALPHA8;
		pixelFormat = GL_RGBA;
		pixelType = GL_UNSIGNED_BYTE;
		return true;
	}
	internalFormat = GL_RGBA;
	pixelFormat = GL_RGBA;
	pixelType = GL_UNSIGNED_BYTE;
	return true;
}

bool usesSrgbFramebuffer(const postprocess::ShaderPass& pass)
{
	const auto isSrgb = [](postprocess::ShaderTargetFormat format, const std::string& formatName) {
		return format == postprocess::ShaderTargetFormat::Srgb || formatName == "R8G8B8A8_SRGB";
	};
	if (pass.outputs.empty())
		return isSrgb(pass.targetFormat, pass.targetFormatName);
	return std::any_of(pass.outputs.begin(), pass.outputs.end(), [&](const postprocess::ShaderPassOutput& output) {
		return isSrgb(output.targetFormat, output.targetFormatName);
	});
}

std::unique_ptr<GlFramebuffer> createTarget(const postprocess::ShaderPass& pass, int width, int height, std::string& error)
{
	GLenum internalFormat;
	GLenum pixelFormat;
	GLenum pixelType;
	if (!glTargetFormat(pass, internalFormat, pixelFormat, pixelType, error))
		return nullptr;
	return std::make_unique<GlFramebuffer>(width, height, false, 0, internalFormat, pixelFormat, pixelType);
}

} // namespace

bool GlShaderChain::ensureMrtTarget(MrtTarget& target, const postprocess::ShaderPass& pass,
		int width, int height, std::string& error)
{
	GLint maxTextureSize = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	if (width <= 0 || height <= 0 || width > maxTextureSize || height > maxTextureSize) {
		error = "OpenGL post-processing target exceeds the device texture-size limit.";
		return false;
	}
	const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
	const bool matching = target.outputs.size() == outputCount && std::all_of(target.outputs.begin(), target.outputs.end(),
			[&](const std::unique_ptr<GlFramebuffer>& output) {
				return output != nullptr && output->getWidth() == width && output->getHeight() == height;
			});
	if (matching && target.framebuffer != 0)
		return true;
	target.initialized = false;
	if (target.framebuffer != 0) {
		glDeleteFramebuffers(1, &target.framebuffer);
		target.framebuffer = 0;
	}
	target.outputs.clear();
	target.outputs.reserve(outputCount);
	for (uint32_t output = 0; output < outputCount; ++output) {
		postprocess::ShaderPass outputPass = pass;
		if (!pass.outputs.empty()) {
			outputPass.targetFormat = pass.outputs[output].targetFormat;
			outputPass.targetFormatName = pass.outputs[output].targetFormatName;
		}
		std::unique_ptr<GlFramebuffer> framebuffer = createTarget(outputPass, width, height, error);
		if (framebuffer == nullptr)
			return false;
		target.outputs.push_back(std::move(framebuffer));
	}
	glGenFramebuffers(1, &target.framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	std::array<GLenum, 8> attachments {};
	for (uint32_t output = 0; output < outputCount; ++output) {
		attachments[output] = GL_COLOR_ATTACHMENT0 + output;
		glFramebufferTexture2D(GL_FRAMEBUFFER, attachments[output], GL_TEXTURE_2D, target.outputs[output]->getTexture(), 0);
	}
	if (outputCount > 1)
	{
#ifdef GLES2
		error = "OpenGL MRT shader passes require OpenGL or GLES 3.0.";
		glDeleteFramebuffers(1, &target.framebuffer);
		target.framebuffer = 0;
		target.outputs.clear();
		return false;
#else
		GLint maxDrawBuffers = 0;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
		if (maxDrawBuffers < static_cast<GLint>(outputCount)) {
			error = "OpenGL does not provide enough draw buffers for this shader pass.";
			glDeleteFramebuffers(1, &target.framebuffer);
			target.framebuffer = 0;
			target.outputs.clear();
			return false;
		}
		glDrawBuffers(outputCount, attachments.data());
#endif
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		error = "OpenGL could not create a complete multi-render-target framebuffer.";
		glDeleteFramebuffers(1, &target.framebuffer);
		target.framebuffer = 0;
		target.outputs.clear();
		return false;
	}
#ifndef GLES2
	if (usesSrgbFramebuffer(pass))
		glcache.Enable(GL_FRAMEBUFFER_SRGB);
	else
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
	glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
#ifndef GLES2
	glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
	return true;
}

GlShaderChain::~GlShaderChain()
{
	term();
}

void GlShaderChain::VertexArray::defineVtxAttribs()
{
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, nullptr);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5,
			reinterpret_cast<void *>(sizeof(float) * 3));
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE1_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_OFFS1_ARRAY);
	glDisableVertexAttribArray(VERTEX_UV1_ARRAY);
}

bool GlShaderChain::ensureResources()
{
	if (vertexBuffer != nullptr)
		return true;

	static constexpr float vertices[][5] = {
		{ -1.f, -1.f, 1.f, 0.f, 0.f },
		{ -1.f,  1.f, 1.f, 0.f, 1.f },
		{  1.f, -1.f, 1.f, 1.f, 0.f },
		{  1.f,  1.f, 1.f, 1.f, 1.f },
	};
	vertexBuffer = std::make_unique<GlBuffer>(GL_ARRAY_BUFFER, GL_STATIC_DRAW);
	vertexBuffer->update(vertices, sizeof(vertices));
	PostProcessSource vertexSource;
	vertexSource.addSource(VertexCompatShader).addSource(FullscreenVertexShader);
	PostProcessSource fragmentSource;
	fragmentSource.addSource(PixelCompatShader)
			.addSource("in mediump vec2 vTexCoord;\nuniform sampler2D Source;\n"
					"void main() { gl_FragColor = texture(Source, vTexCoord); }\n");
	copyProgram = gl_CompileAndLink(vertexSource.generate().c_str(), fragmentSource.generate().c_str());
	copySourceUniform = glGetUniformLocation(copyProgram, "Source");
	PostProcessSource rotatedVertexSource;
	rotatedVertexSource.addSource(VertexCompatShader).addSource(RotatedFullscreenVertexShader);
	copyRotateProgram = gl_CompileAndLink(rotatedVertexSource.generate().c_str(), fragmentSource.generate().c_str());
	copyRotateSourceUniform = glGetUniformLocation(copyRotateProgram, "Source");
	return true;
}

void GlShaderChain::deletePrograms(std::vector<Program>& programs)
{
	for (Program& program : programs) {
#ifndef GLES2
		if (glDeleteSamplers != nullptr) {
			for (ResourceUniform& resource : program.resources) {
				if (resource.sampler != 0) glDeleteSamplers(1, &resource.sampler);
				if (resource.mipSampler != 0) glDeleteSamplers(1, &resource.mipSampler);
			}
		}
#endif
		if (program.handle != 0)
			glcache.DeleteProgram(program.handle);
	}
	programs.clear();
}

void GlShaderChain::deleteLookupTextures()
{
	deleteLookupTextures(lookupTextures);
}

void GlShaderChain::deleteLookupTextures(std::vector<LookupTexture>& textures)
{
	for (LookupTexture& lookup : textures)
		if (lookup.texture != 0)
			glcache.DeleteTextures(1, &lookup.texture);
	textures.clear();
}

bool GlShaderChain::ensureLookupTextures(std::string& error)
{
	return ensureLookupTextures(lookupTextures, error);
}

bool GlShaderChain::ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error)
{
	uint64_t lookupBytes = 0;
	for (LookupTexture& lookup : textures) {
		if (lookup.texture != 0)
			continue;
		std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(lookup.config.path, "rb"));
		if (!file) {
			error = "Unable to open post-processing lookup texture: " + lookup.config.path;
			return false;
		}
		const s64 fileSize = file->size();
		if (fileSize < 0 || static_cast<uint64_t>(fileSize) > postprocess::MaxShaderLookupFileBytes) {
			error = "Post-processing lookup texture exceeds the 64 MiB file limit: " + lookup.config.path;
			return false;
		}
		int width = 0;
		int height = 0;
		int channels = 0;
		u8 *pixels = stbi_load_from_file(file.get(), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels || width <= 0 || height <= 0) {
			stbi_image_free(pixels);
			error = "Unable to decode post-processing lookup texture: " + lookup.config.path;
			return false;
		}
		if (!postprocess::validateShaderLookupTexture(width, height, error)) {
			stbi_image_free(pixels);
			return false;
		}
		if (!postprocess::accumulateShaderTargetBytes(width, height,
				postprocess::ShaderTargetFormat::Default, {}, lookup.config.useMipmaps,
				1, lookupBytes, error)) {
			stbi_image_free(pixels);
			return false;
		}
		glGenTextures(1, &lookup.texture);
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		glcache.BindTexture(GL_TEXTURE_2D, lookup.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		stbi_image_free(pixels);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				lookup.config.useMipmaps
						? (lookup.config.linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
						: lookup.config.linearFilter ? GL_LINEAR : GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lookup.config.linearFilter ? GL_LINEAR : GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapMode(lookup.config.wrapMode));
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapMode(lookup.config.wrapMode));
		if (lookup.config.useMipmaps)
			glGenerateMipmap(GL_TEXTURE_2D);
	}
	return true;
}

bool GlShaderChain::compileCandidate(const postprocess::ShaderChain& chain,
		std::vector<Program>& candidate, std::string& error)
{
	if (chain.passes.empty()) {
		error = "A shader chain must contain at least one pass.";
		return false;
	}

	GLint textureUnitLimit = 0;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnitLimit);
	if (textureUnitLimit <= 0) {
		error = "OpenGL did not report usable fragment texture units.";
		return false;
	}

	candidate.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		const postprocess::ShaderPass& pass = chain.passes[passIndex];
		postprocess::ShaderPassSource loadedSource;
		const postprocess::ShaderPassSource *passSource = passIndex < chain.sources.size()
				? &chain.sources[passIndex] : &loadedSource;
		if (passSource == &loadedSource && !postprocess::loadShaderPassSource(pass, loadedSource, error)) {
			deletePrograms(candidate);
			return false;
		}
		std::string shaderBody = passSource->fragment;
		removeVersionDirective(shaderBody);
		removeResourceDeclarations(shaderBody);
		std::string vertexBody = passSource->vertex;
		removeVersionDirective(vertexBody);
		removeResourceDeclarations(vertexBody);
		const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
		const bool supportsExplicitMrtLocations = gl.is_gles ? gl.gl_major >= 3
				: gl.gl_major > 3 || (gl.gl_major == 3 && gl.gl_minor >= 3);
		if (outputCount > 1 && !supportsExplicitMrtLocations) {
			error = "OpenGL MRT shader passes require OpenGL 3.3 or GLES 3.0.";
			deletePrograms(candidate);
			return false;
		}

		try {
			std::vector<postprocess::ShaderParameter> passParameters;
			std::copy_if(chain.parameters.begin(), chain.parameters.end(), std::back_inserter(passParameters),
					[&](const postprocess::ShaderParameter& parameter) {
						return parameter.layerIndex == pass.layerIndex;
					});
			PostProcessSource vertexSource;
			vertexSource.addSource(VertexCompatShader);
			if (vertexBody.empty())
				vertexSource.addSource(FullscreenVertexShader);
			else {
				// Slang's common vertex names are aliases for HollyCast's fixed fullscreen VBO.
				// This preserves custom transforms without asking shader packages for backend attributes.
				vertexSource.addSource(passSource->retroArchGlsl
						? "in highp vec3 in_pos;\nin mediump vec2 in_uv;\n"
						  "#define VertexCoord vec4(in_pos, 1.0)\n#define Position VertexCoord\n#define TexCoord in_uv\n#define aTexCoord in_uv\n"
						: "#define VertexCoord in_pos\n#define Position in_pos\n#define TexCoord in_uv\n#define aTexCoord in_uv\n")
						.addSource(vertexResourcePreamble(passSource->slangResources))
						.addSource(vertexBody);
			}
			PostProcessSource fragmentSource;
			fragmentSource.addSource(PixelCompatShader)
					.addSource(fragmentPreamble(passParameters, passSource->slangParameters,
							passSource->slangResources, shaderBody, outputCount))
					.addSource(shaderBody);
			Program program;
			program.duckStationGlsl = passSource->duckStationGlsl;
			const std::string vertexShader = vertexSource.generate();
			program.handle = gl_CompileAndLink(vertexShader.c_str(), fragmentSource.generate().c_str(), &error);
			program.pass = pass;
			program.sourceUniform = glGetUniformLocation(program.handle, "Source");
			program.sourceSizeUniform = glGetUniformLocation(program.handle, "SourceSize");
			program.originalSizeUniform = glGetUniformLocation(program.handle, "OriginalSize");
			program.outputSizeUniform = glGetUniformLocation(program.handle, "OutputSize");
			program.frameCountUniform = glGetUniformLocation(program.handle, "FrameCount");
			program.rotationUniform = glGetUniformLocation(program.handle, "Rotation");
			for (const postprocess::ShaderParameter& parameter : passParameters)
				program.parameters.push_back({ glGetUniformLocation(program.handle, parameter.name.c_str()), parameter.name,
						parameter.layerIndex, parameter.type, parameter.values });
			for (const postprocess::SlangParameter& parameter : passSource->slangParameters) {
				if (std::any_of(program.parameters.begin(), program.parameters.end(), [&](const ParameterUniform& value) {
					return value.name == parameter.name;
				}))
					continue;
				program.parameters.push_back({ glGetUniformLocation(program.handle, parameter.name.c_str()), parameter.name,
						pass.layerIndex, parameter.type, parameter.defaultValue.values });
			}

			GLuint textureUnit = 0;
			bool hasSource = false;
			for (const postprocess::SlangResource& resource : passSource->slangResources) {
				if (!isTexture2D(resource.type) || resource.arrayCount != 1) {
					error = "OpenGL supports only single sampler2D resources; '" + resource.name + "' is unsupported.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				if (textureUnit >= static_cast<GLuint>(textureUnitLimit)) {
					error = "Shader pass requires more texture units than this OpenGL device supports.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				auto input = std::find_if(pass.inputs.begin(), pass.inputs.end(), [&](const postprocess::ShaderPassInput& value) {
					return value.bindingName == resource.name;
				});
				if (input == pass.inputs.end()) {
					error = "Shader resource '" + resource.name + "' has no resolved graph input.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				uint32_t lookupIndex = postprocess::InvalidShaderPass;
				if (input->kind == postprocess::ShaderInputKind::LookupTexture) {
					const auto lookup = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const postprocess::ShaderPresetTexture& texture) {
						return texture.name == input->resourceName;
					});
					if (lookup == chain.textures.end()) {
						error = "Shader lookup texture '" + input->resourceName + "' was not resolved.";
						glcache.DeleteProgram(program.handle);
						deletePrograms(candidate);
						return false;
					}
					lookupIndex = static_cast<uint32_t>(std::distance(chain.textures.begin(), lookup));
				}
				ResourceUniform binding { glGetUniformLocation(program.handle, resource.name.c_str()), input->kind,
						input->sourcePass, input->sourceOutput, input->historyOffset, input->resourceName,
						lookupIndex, textureUnit++ };
				const auto samplerConfig = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const auto& sampler) {
					return sampler.bindingName == resource.name;
				});
				if (samplerConfig != pass.samplers.end() && input->kind != postprocess::ShaderInputKind::LookupTexture) {
					binding.hasSamplerState = true;
					binding.linearFilter = samplerConfig->linearFilter;
					binding.useMipmaps = samplerConfig->useMipmaps;
					binding.wrapMode = samplerConfig->wrapMode;
				}
				if (resource.name == "Source") {
					hasSource = true;
				}
				program.resources.push_back(binding);
			}
			if (!hasSource) {
				if (textureUnit >= static_cast<GLuint>(textureUnitLimit)) {
					error = "Shader pass requires more texture units than this OpenGL device supports.";
					glcache.DeleteProgram(program.handle);
					deletePrograms(candidate);
					return false;
				}
				program.resources.push_back({ program.sourceUniform, postprocess::ShaderInputKind::Source,
						postprocess::InvalidShaderPass, 0, 1, {}, postprocess::InvalidShaderPass, textureUnit });
			}
#ifndef GLES2
			if (glGenSamplers != nullptr && glSamplerParameteri != nullptr) {
				for (ResourceUniform& binding : program.resources) {
					if (!binding.hasSamplerState)
						continue;
					glGenSamplers(1, &binding.sampler);
					glGenSamplers(1, &binding.mipSampler);
					if (binding.sampler == 0 || binding.mipSampler == 0) {
						if (glDeleteSamplers != nullptr)
							for (ResourceUniform& allocated : program.resources) {
								if (allocated.sampler != 0) glDeleteSamplers(1, &allocated.sampler);
								if (allocated.mipSampler != 0) glDeleteSamplers(1, &allocated.mipSampler);
							}
						error = "OpenGL could not create ReShade sampler state.";
						glcache.DeleteProgram(program.handle);
						deletePrograms(candidate);
						return false;
					}
					const GLint wrap = glWrapMode(binding.wrapMode);
					for (const GLuint sampler : { binding.sampler, binding.mipSampler }) {
						glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, binding.linearFilter ? GL_LINEAR : GL_NEAREST);
						glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap);
						glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap);
					}
					glSamplerParameteri(binding.sampler, GL_TEXTURE_MIN_FILTER,
							binding.linearFilter ? GL_LINEAR : GL_NEAREST);
					glSamplerParameteri(binding.mipSampler, GL_TEXTURE_MIN_FILTER,
							binding.linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
				}
			}
#endif
			candidate.push_back(std::move(program));
		} catch (const RendererException& exception) {
			if (error.empty())
				error = exception.what();
			deletePrograms(candidate);
			return false;
		}
	}
	const std::vector<std::vector<bool>> mipmapRequirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	for (size_t passIndex = 0; passIndex < candidate.size(); ++passIndex)
		candidate[passIndex].outputNeedsMipmaps = mipmapRequirements[passIndex];
	return true;
}

bool GlShaderChain::activate(const postprocess::ShaderChain& chain, std::string& error)
{
	std::vector<Program> candidate;
	if (!compileCandidate(chain, candidate, error))
		return false;
	std::vector<LookupTexture> candidateLookupTextures;
	candidateLookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		candidateLookupTextures.push_back({ texture });
	if (!ensureLookupTextures(candidateLookupTextures, error)) {
		deletePrograms(candidate);
		deleteLookupTextures(candidateLookupTextures);
		return false;
	}

	deletePrograms(programs);
	deleteLookupTextures();
	programs = std::move(candidate);
	lookupTextures = std::move(candidateLookupTextures);
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			if (input.kind != postprocess::ShaderInputKind::History
					&& input.kind != postprocess::ShaderInputKind::Feedback)
				continue;
			const uint32_t requiredDepth = input.kind == postprocess::ShaderInputKind::Feedback
					? 2u : input.historyOffset + 1;
			if (input.sourcePass == postprocess::InvalidShaderPass) {
				sourceHistoryDepth = std::max(sourceHistoryDepth, requiredDepth);
				needsSourceHistory = true;
			}
			else {
				programs[input.sourcePass].historyDepth =
						std::max(programs[input.sourcePass].historyDepth, requiredDepth);
			}
		}
	targets.clear();
	displayInput.reset();
	mipmapInput.reset();
	sourceHistory.clear();
	error.clear();
	return true;
}

void GlShaderChain::updateConfiguredChain()
{
	const std::vector<std::string> requestedPaths = postprocess::configuredShaderPaths();
	const std::string requestedLabel = postprocess::shaderSelectionLabel(requestedPaths);
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPaths == configuredPaths && generation == configuredGeneration)
		return;
	configuredPaths = requestedPaths;
	configuredGeneration = generation;

	if (requestedPaths.empty()) {
		deletePrograms(programs);
		deleteLookupTextures();
		targets.clear();
		displayInput.reset();
		mipmapInput.reset();
		sourceHistory.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return;
	}

	std::string error;
	postprocess::ShaderChain candidate;
	if (!postprocess::loadShaderChain(requestedPaths, candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedLabel.c_str(), error.c_str());
		return;
	}
	postprocess::applyShaderChainParameterValues(candidate, config::PostProcessShaderParameters.get());
	if (!activate(candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return;
	}
	activePath = requestedLabel;
	postprocess::reportShaderChainStatus(activePath, {}, candidate.parameters,
			candidate.techniques, candidate.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
}

GlFramebuffer *GlShaderChain::render(GLuint sourceTexture, const postprocess::ShaderFrameInfo& frame,
		bool& presentationTransformApplied)
{
	presentationTransformApplied = false;
	updateConfiguredChain();
	if (programs.empty() || sourceTexture == 0 || !ensureResources())
		return nullptr;
	std::string lookupError;
	if (!ensureLookupTextures(lookupError)) {
		postprocess::reportShaderChainStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "Post-processing lookup texture was not loaded: %s", lookupError.c_str());
		return nullptr;
	}

	postprocess::ShaderFrameInfo passFrame = frame;
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty()) {
			for (Program& program : programs)
				for (ParameterUniform& parameter : program.parameters) {
					auto configured = std::find_if(status.parameters.begin(), status.parameters.end(), [&](const postprocess::ShaderParameter& value) {
						return value.name == parameter.name && value.layerIndex == parameter.layerIndex;
					});
					if (configured != status.parameters.end())
						std::copy_n(configured->values.begin(), configured->componentCount, parameter.values.begin());
				}
		}
	}
	if (targets.size() != programs.size())
		targets.resize(programs.size());
	GLuint inputTexture = sourceTexture;
	GLuint originalTexture = sourceTexture;
	bool inputHasMipmaps = false;
	struct SourceTextureState {
		GLuint texture;
		bool captured = false;
		GLint minFilter = GL_LINEAR;
		GLint magFilter = GL_LINEAR;
		GLint wrapS = GL_CLAMP_TO_EDGE;
		GLint wrapT = GL_CLAMP_TO_EDGE;
		void capture()
		{
			if (captured)
				return;
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, texture);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);
			captured = true;
		}
		~SourceTextureState()
		{
			if (!captured)
				return;
			glActiveTexture(GL_TEXTURE0);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, texture);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
		}
	} sourceTextureState { sourceTexture };
	bool displaySpaceReached = false;
	int originalWidth = passFrame.sourceWidth;
	int originalHeight = passFrame.sourceHeight;
	auto composeDisplayInput = [&]() {
		if (displaySpaceReached)
			return true;
		displaySpaceReached = true;
		if (!postprocess::shouldComposeDisplayInput(frame, false))
			return true;
		if (displayInput == nullptr || displayInput->getWidth() != frame.viewportWidth
				|| displayInput->getHeight() != frame.viewportHeight)
			displayInput = std::make_unique<GlFramebuffer>(frame.viewportWidth, frame.viewportHeight, false, true);
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);
#ifndef GLES2
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
		displayInput->bind();
		glViewport(0, 0, frame.viewportWidth, frame.viewportHeight);
		glcache.UseProgram(frame.rotationPending ? copyRotateProgram : copyProgram);
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		glcache.BindTexture(GL_TEXTURE_2D, inputTexture);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, config::LinearInterpolation ? GL_LINEAR : GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, config::LinearInterpolation ? GL_LINEAR : GL_NEAREST);
		const GLint sourceUniform = frame.rotationPending ? copyRotateSourceUniform : copySourceUniform;
		if (sourceUniform != -1)
			glUniform1i(sourceUniform, 0);
		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
		inputTexture = displayInput->getTexture();
		originalTexture = inputTexture;
		inputHasMipmaps = false;
		passFrame.sourceWidth = frame.viewportWidth;
		passFrame.sourceHeight = frame.viewportHeight;
		originalWidth = passFrame.sourceWidth;
		originalHeight = passFrame.sourceHeight;
		presentationTransformApplied = true;
		return true;
	};
	if (programs.front().pass.composeDisplayInput && !composeDisplayInput())
		return nullptr;

	postprocess::ShaderFrameInfo sizeFrame = frame;
	bool sizeDisplaySpaceReached = false;
	uint64_t targetBytes = 0;
	std::string targetBudgetError;
	const auto displayBoundary = std::find_if(programs.begin(), programs.end(), [](const Program& program) {
		return program.pass.composeDisplayInput;
	});
	if (displayBoundary != programs.end() && postprocess::shouldComposeDisplayInput(frame, false)
			&& !postprocess::accumulateShaderTargetBytes(frame.viewportWidth, frame.viewportHeight,
					postprocess::ShaderTargetFormat::Default, {}, displayBoundary->pass.useMipmaps,
					1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderChainStatus(activePath, targetBudgetError);
		return nullptr;
	}
	if (programs.front().pass.useMipmaps
			&& !(programs.front().pass.composeDisplayInput && postprocess::shouldComposeDisplayInput(frame, false))
			&& !postprocess::accumulateShaderTargetBytes(frame.sourceWidth, frame.sourceHeight,
					postprocess::ShaderTargetFormat::Default, {}, true, 1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderChainStatus(activePath, targetBudgetError);
		return nullptr;
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		if (programs[index].pass.composeDisplayInput && !sizeDisplaySpaceReached) {
			sizeDisplaySpaceReached = true;
			if (postprocess::shouldComposeDisplayInput(frame, false)) {
				sizeFrame.sourceWidth = frame.viewportWidth;
				sizeFrame.sourceHeight = frame.viewportHeight;
			}
		}
		std::string error;
		if (!postprocess::resolveTargetSize(programs[index].pass, sizeFrame,
				programs[index].targetWidth, programs[index].targetHeight, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			return nullptr;
		}
		sizeFrame.sourceWidth = programs[index].targetWidth;
		sizeFrame.sourceHeight = programs[index].targetHeight;
		const uint32_t outputCount = programs[index].pass.outputs.empty()
				? 1u : static_cast<uint32_t>(programs[index].pass.outputs.size());
		for (uint32_t output = 0; output < outputCount; ++output) {
			const postprocess::ShaderPassOutput config = programs[index].pass.outputs.empty()
					? postprocess::ShaderPassOutput {} : programs[index].pass.outputs[output];
			const postprocess::ShaderTargetFormat format = programs[index].pass.outputs.empty()
					? programs[index].pass.targetFormat : config.targetFormat;
			const std::string& formatName = programs[index].pass.outputs.empty()
					? programs[index].pass.targetFormatName : config.targetFormatName;
			if (!postprocess::accumulateShaderTargetBytes(programs[index].targetWidth, programs[index].targetHeight,
					format, formatName, programs[index].outputNeedsMipmaps[output],
					programs[index].historyDepth, targetBytes, targetBudgetError)) {
				postprocess::reportShaderChainStatus(activePath, targetBudgetError);
				return nullptr;
			}
		}
		MrtTargetRing& targetRing = targets[index];
		if (targetRing.size() != programs[index].historyDepth)
			targetRing.resize(programs[index].historyDepth);
		for (std::unique_ptr<MrtTarget>& target : targetRing) {
			if (target == nullptr)
				target = std::make_unique<MrtTarget>();
			if (!ensureMrtTarget(*target, programs[index].pass, programs[index].targetWidth,
					programs[index].targetHeight, error)) {
				postprocess::reportShaderChainStatus(activePath, error);
				return nullptr;
			}
		}
	}
	if (needsSourceHistory) {
		if (!postprocess::accumulateShaderTargetBytes(passFrame.sourceWidth, passFrame.sourceHeight,
				postprocess::ShaderTargetFormat::Default, {}, false, sourceHistoryDepth,
				targetBytes, targetBudgetError)) {
			postprocess::reportShaderChainStatus(activePath, targetBudgetError);
			return nullptr;
		}
		if (sourceHistory.size() != sourceHistoryDepth)
			sourceHistory.resize(sourceHistoryDepth);
		for (HistoryTarget& target : sourceHistory)
			if (target.framebuffer == nullptr || target.framebuffer->getWidth() != passFrame.sourceWidth
					|| target.framebuffer->getHeight() != passFrame.sourceHeight) {
				target.framebuffer = std::make_unique<GlFramebuffer>(passFrame.sourceWidth, passFrame.sourceHeight, false, true);
				target.initialized = false;
				target.framebuffer->bind();
				glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
		const size_t currentSourceTarget = frame.frameCount % sourceHistoryDepth;
		HistoryTarget& historyTarget = sourceHistory[currentSourceTarget];
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);
#ifndef GLES2
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
		historyTarget.framebuffer->bind();
		glViewport(0, 0, passFrame.sourceWidth, passFrame.sourceHeight);
		glcache.UseProgram(copyProgram);
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		glcache.BindTexture(GL_TEXTURE_2D, originalTexture);
		if (copySourceUniform != -1)
			glUniform1i(copySourceUniform, 0);
		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
		historyTarget.renderedFrame = frame.frameCount;
		historyTarget.initialized = true;
	}
	GlFramebuffer *output = nullptr;
	const auto disableFramebufferSrgb = [] {
#ifndef GLES2
		glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
	};
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		if (program.pass.composeDisplayInput && !composeDisplayInput())
			return nullptr;
		if (program.pass.useMipmaps && !inputHasMipmaps) {
			if (inputTexture == sourceTexture) {
				if (mipmapInput == nullptr || mipmapInput->getWidth() != passFrame.sourceWidth
						|| mipmapInput->getHeight() != passFrame.sourceHeight)
					mipmapInput = std::make_unique<GlFramebuffer>(passFrame.sourceWidth, passFrame.sourceHeight, false, true);
				glcache.Disable(GL_SCISSOR_TEST);
				glcache.Disable(GL_DEPTH_TEST);
				glcache.Disable(GL_STENCIL_TEST);
				glcache.Disable(GL_CULL_FACE);
				glcache.Disable(GL_BLEND);
#ifndef GLES2
				glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
				mipmapInput->bind();
				glViewport(0, 0, passFrame.sourceWidth, passFrame.sourceHeight);
				glcache.UseProgram(copyProgram);
				glActiveTexture(GL_TEXTURE0);
				sourceTextureState.capture();
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				if (copySourceUniform != -1)
					glUniform1i(copySourceUniform, 0);
				vertexArray.bind(vertexBuffer.get());
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				GlVertexArray::unbind();
				inputTexture = mipmapInput->getTexture();
			}
			glActiveTexture(GL_TEXTURE0);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, inputTexture);
			glGenerateMipmap(GL_TEXTURE_2D);
			inputHasMipmaps = true;
		}
		const size_t currentTarget = frame.frameCount % program.historyDepth;
		const int outputWidth = program.targetWidth;
		const int outputHeight = program.targetHeight;
		std::string error;
		MrtTargetRing& targetRing = targets[index];
		MrtTarget *target = targetRing[currentTarget].get();

		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
#ifndef GLES2
		if (usesSrgbFramebuffer(program.pass))
			glcache.Enable(GL_FRAMEBUFFER_SRGB);
		else
			glcache.Disable(GL_FRAMEBUFFER_SRGB);
#endif
		glViewport(0, 0, outputWidth, outputHeight);
		if (program.pass.blendEnabled) {
			// ReShade blends against the current backbuffer. Seed HollyCast's
			// private pass target with the current input before applying the pass.
			glcache.Disable(GL_BLEND);
			glcache.UseProgram(copyProgram);
			glActiveTexture(GL_TEXTURE0);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, inputTexture);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					program.pass.linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					program.pass.linearFilter ? GL_LINEAR : GL_NEAREST);
			if (copySourceUniform != -1)
				glUniform1i(copySourceUniform, 0);
			vertexArray.bind(vertexBuffer.get());
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			GlVertexArray::unbind();
			glcache.Enable(GL_BLEND);
			glcache.BlendFunc(glBlendFactor(program.pass.sourceBlend), glBlendFactor(program.pass.destinationBlend));
			glcache.BlendEquation(glBlendOperation(program.pass.blendOperation));
		}
		else
			glcache.Disable(GL_BLEND);
		glcache.UseProgram(program.handle);
		for (const ResourceUniform& resource : program.resources) {
			GLuint texture = inputTexture;
			const LookupTexture *lookup = nullptr;
			bool resourceHasMipmaps = false;
			if ((resource.kind == postprocess::ShaderInputKind::PassOutput
						|| resource.kind == postprocess::ShaderInputKind::History
						|| resource.kind == postprocess::ShaderInputKind::Feedback)
					&& resource.sourcePass != postprocess::InvalidShaderPass
					&& (resource.sourcePass >= targets.size()
							|| targets[resource.sourcePass].empty()
							|| resource.sourceOutput >= targets[resource.sourcePass].front()->outputs.size())) {
				error = "OpenGL shader graph references an invalid pass output.";
				postprocess::reportShaderChainStatus(activePath, error);
				disableFramebufferSrgb();
				return nullptr;
			}
			switch (resource.kind) {
			case postprocess::ShaderInputKind::Source:
				resourceHasMipmaps = inputHasMipmaps;
				break;
			case postprocess::ShaderInputKind::Original:
				texture = originalTexture;
				break;
			case postprocess::ShaderInputKind::PassOutput: {
				MrtTarget *sourceTarget = targets[resource.sourcePass]
						[frame.frameCount % programs[resource.sourcePass].historyDepth].get();
				if (!sourceTarget->initialized || sourceTarget->renderedFrame != frame.frameCount) {
					error = "OpenGL shader pass input was not rendered for the current frame.";
					postprocess::reportShaderChainStatus(activePath, error);
					disableFramebufferSrgb();
					return nullptr;
				}
				texture = sourceTarget->outputs[resource.sourceOutput]->getTexture();
				resourceHasMipmaps = programs[resource.sourcePass].outputNeedsMipmaps[resource.sourceOutput];
				break;
			}
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback:
			{
				const uint32_t offset = resource.kind == postprocess::ShaderInputKind::Feedback ? 1 : resource.historyOffset;
				if (frame.frameCount < offset) {
					texture = originalTexture;
					break;
				}
				const uint32_t requestedFrame = frame.frameCount - offset;
				if (resource.sourcePass == postprocess::InvalidShaderPass) {
					const HistoryTarget& history = sourceHistory[requestedFrame % sourceHistoryDepth];
					texture = history.initialized && history.renderedFrame == requestedFrame
							? history.framebuffer->getTexture() : originalTexture;
				}
				else {
					const uint32_t sourceDepth = programs[resource.sourcePass].historyDepth;
					const MrtTarget *history = targets[resource.sourcePass][requestedFrame % sourceDepth].get();
					texture = history->initialized && history->renderedFrame == requestedFrame
							? history->outputs[resource.sourceOutput]->getTexture() : originalTexture;
					resourceHasMipmaps = history->initialized && history->renderedFrame == requestedFrame
							&& programs[resource.sourcePass].outputNeedsMipmaps[resource.sourceOutput];
				}
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture:
				if (resource.lookupIndex >= lookupTextures.size()) {
					disableFramebufferSrgb();
					return nullptr;
				}
				lookup = &lookupTextures[resource.lookupIndex];
				texture = lookup->texture;
				resourceHasMipmaps = lookup->config.useMipmaps;
				break;
			}
			glActiveTexture(GL_TEXTURE0 + resource.unit);
			glcache.InvalidateTextureBinding();
			glcache.BindTexture(GL_TEXTURE_2D, texture);
			if (texture == sourceTexture)
				sourceTextureState.capture();
			const bool linearFilter = lookup != nullptr ? lookup->config.linearFilter
					: resource.hasSamplerState ? resource.linearFilter : program.pass.linearFilter;
			const bool sampleMipmaps = resourceHasMipmaps && (lookup != nullptr ? lookup->config.useMipmaps
					: resource.hasSamplerState ? resource.useMipmaps : true);
			const postprocess::ShaderWrapMode wrapMode = lookup != nullptr ? lookup->config.wrapMode
					: resource.hasSamplerState ? resource.wrapMode : postprocess::ShaderWrapMode::ClampToEdge;
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					sampleMipmaps ? (linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
							: linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapMode(wrapMode));
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapMode(wrapMode));
#ifndef GLES2
			if (glBindSampler != nullptr)
				glBindSampler(resource.unit, resource.sampler == 0 ? 0
						: sampleMipmaps ? resource.mipSampler : resource.sampler);
#endif
			if (resource.location != -1)
				glUniform1i(resource.location, static_cast<GLint>(resource.unit));
		}
		glActiveTexture(GL_TEXTURE0);
		glcache.InvalidateTextureBinding();
		if (program.sourceSizeUniform != -1)
			glUniform2f(program.sourceSizeUniform, static_cast<float>(passFrame.sourceWidth), static_cast<float>(passFrame.sourceHeight));
		if (program.originalSizeUniform != -1)
			glUniform2f(program.originalSizeUniform,
					static_cast<float>(program.duckStationGlsl ? frame.sourceWidth : originalWidth),
					static_cast<float>(program.duckStationGlsl ? frame.sourceHeight : originalHeight));
		if (program.outputSizeUniform != -1)
			glUniform2f(program.outputSizeUniform, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
		if (program.frameCountUniform != -1)
			glUniform1i(program.frameCountUniform, static_cast<GLint>(program.pass.frameCountModulo == 0
					? frame.frameCount : frame.frameCount % program.pass.frameCountModulo));
		if (program.rotationUniform != -1)
			glUniform1i(program.rotationUniform, frame.rotationPending ? 1 : 0);
		for (const ParameterUniform& parameter : program.parameters) {
			if (parameter.location == -1)
				continue;
			switch (parameter.type) {
			case postprocess::SlangParameterType::Float: glUniform1f(parameter.location, parameter.values[0]); break;
			case postprocess::SlangParameterType::Int: glUniform1i(parameter.location, static_cast<GLint>(parameter.values[0])); break;
			case postprocess::SlangParameterType::Bool: glUniform1i(parameter.location, parameter.values[0] != 0.f); break;
			case postprocess::SlangParameterType::Float2: glUniform2fv(parameter.location, 1, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float3: glUniform3fv(parameter.location, 1, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float4: glUniform4fv(parameter.location, 1, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float2x2: glUniformMatrix2fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float3x3: glUniformMatrix3fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			case postprocess::SlangParameterType::Float4x4: glUniformMatrix4fv(parameter.location, 1, GL_FALSE, parameter.values.data()); break;
			}
		}

		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
#ifndef GLES2
		if (glBindSampler != nullptr)
			for (const ResourceUniform& resource : program.resources)
				glBindSampler(resource.unit, 0);
#endif
		for (uint32_t outputIndex = 0; outputIndex < target->outputs.size(); ++outputIndex) {
			if (program.outputNeedsMipmaps[outputIndex]) {
				glActiveTexture(GL_TEXTURE0);
				glcache.InvalidateTextureBinding();
				glcache.BindTexture(GL_TEXTURE_2D, target->outputs[outputIndex]->getTexture());
				glGenerateMipmap(GL_TEXTURE_2D);
			}
		}

		target->renderedFrame = frame.frameCount;
		target->initialized = true;
		inputTexture = target->outputs[0]->getTexture();
		inputHasMipmaps = program.outputNeedsMipmaps[0];
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
		output = target->outputs[0].get();
	}
	disableFramebufferSrgb();
	glCheck();
	return output;
}

void GlShaderChain::term()
{
	deletePrograms(programs);
	deleteLookupTextures();
	targets.clear();
	displayInput.reset();
	mipmapInput.reset();
	sourceHistory.clear();
	vertexBuffer.reset();
	if (copyProgram != 0)
		glcache.DeleteProgram(copyProgram);
	if (copyRotateProgram != 0)
		glcache.DeleteProgram(copyRotateProgram);
	copyProgram = 0;
	copyRotateProgram = 0;
	copySourceUniform = -1;
	copyRotateSourceUniform = -1;
	vertexArray.term();
	configuredPaths.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	activePath.clear();
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	postprocess::reportShaderChainStatus({}, {});
}
