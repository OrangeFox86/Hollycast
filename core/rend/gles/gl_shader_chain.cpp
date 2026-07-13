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
#include <unordered_map>

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
		const std::vector<postprocess::SlangParameter>& slangParameters, const std::string& source, uint32_t outputCount)
{
	std::string preamble;
	if (outputCount == 1)
		preamble = "#define FxOutput0 gl_FragColor\n";
	else
		for (uint32_t output = 0; output < outputCount; ++output)
			preamble += "layout(location = " + std::to_string(output) + ") out vec4 FxOutput" + std::to_string(output) + ";\n";
	if (!hasInputDeclaration(source, "vTexCoord"))
		preamble += "in mediump vec2 vTexCoord;\n";
	if (!hasUniformDeclaration(source, "Source")) preamble += "uniform sampler2D Source;\n";
	if (!hasUniformDeclaration(source, "SourceSize")) preamble += "uniform mediump vec2 SourceSize;\n";
	if (!hasUniformDeclaration(source, "OutputSize")) preamble += "uniform mediump vec2 OutputSize;\n";
	if (!hasUniformDeclaration(source, "FrameCount")) preamble += "uniform int FrameCount;\n";
	for (const postprocess::ShaderParameter& parameter : parameters)
		if (!hasUniformDeclaration(source, parameter.name))
			preamble += "uniform float " + parameter.name + ";\n";
	for (const postprocess::SlangParameter& parameter : slangParameters)
		if (!hasUniformDeclaration(source, parameter.name))
			preamble += "uniform " + std::string(glslType(parameter.type)) + " " + parameter.name + ";\n";
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
		// GLES does not guarantee border-clamp support, so edge clamp is the
		// portable equivalent used for package lookup textures on every GL target.
		return GL_CLAMP_TO_EDGE;
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
	const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
	const bool matching = target.outputs.size() == outputCount && std::all_of(target.outputs.begin(), target.outputs.end(),
			[&](const std::unique_ptr<GlFramebuffer>& output) {
				return output != nullptr && output->getWidth() == width && output->getHeight() == height;
			});
	if (matching && target.framebuffer != 0)
		return true;
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
		return false;
#else
		GLint maxDrawBuffers = 0;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
		if (maxDrawBuffers < static_cast<GLint>(outputCount)) {
			error = "OpenGL does not provide enough draw buffers for this shader pass.";
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
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
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
	OpenGlSource vertexSource;
	vertexSource.addSource(VertexCompatShader).addSource(FullscreenVertexShader);
	OpenGlSource fragmentSource;
	fragmentSource.addSource(PixelCompatShader)
			.addSource("in mediump vec2 vTexCoord;\nuniform sampler2D Source;\n"
					"void main() { gl_FragColor = texture(Source, vTexCoord); }\n");
	copyProgram = gl_CompileAndLink(vertexSource.generate().c_str(), fragmentSource.generate().c_str());
	copySourceUniform = glGetUniformLocation(copyProgram, "Source");
	return true;
}

void GlShaderChain::deletePrograms(std::vector<Program>& programs)
{
	for (Program& program : programs)
		if (program.handle != 0)
			glcache.DeleteProgram(program.handle);
	programs.clear();
}

void GlShaderChain::deleteLookupTextures()
{
	for (LookupTexture& lookup : lookupTextures)
		if (lookup.texture != 0)
			glcache.DeleteTextures(1, &lookup.texture);
	lookupTextures.clear();
}

bool GlShaderChain::ensureLookupTextures(std::string& error)
{
	for (LookupTexture& lookup : lookupTextures) {
		if (lookup.texture != 0)
			continue;
		std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(lookup.config.path, "rb"));
		if (!file) {
			error = "Unable to open post-processing lookup texture: " + lookup.config.path;
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
		glGenTextures(1, &lookup.texture);
		glcache.BindTexture(GL_TEXTURE_2D, lookup.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		stbi_image_free(pixels);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				lookup.config.useMipmaps ? GL_LINEAR_MIPMAP_LINEAR : lookup.config.linearFilter ? GL_LINEAR : GL_NEAREST);
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
		std::string vertexBody = passSource->vertex;
		removeVersionDirective(vertexBody);
		const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
		if (outputCount > 1 && gl.gl_major < 3) {
			error = "OpenGL MRT shader passes require OpenGL or GLES 3.0.";
			deletePrograms(candidate);
			return false;
		}

		try {
			OpenGlSource vertexSource;
			vertexSource.addSource(VertexCompatShader);
			if (vertexBody.empty())
				vertexSource.addSource(FullscreenVertexShader);
			else {
				// Slang's common vertex names are aliases for HollyCast's fixed fullscreen VBO.
				// This preserves custom transforms without asking shader packages for backend attributes.
				vertexSource.addSource("#define VertexCoord in_pos\n#define Position in_pos\n#define TexCoord in_uv\n#define aTexCoord in_uv\n").addSource(vertexBody);
			}
			OpenGlSource fragmentSource;
			fragmentSource.addSource(PixelCompatShader)
					.addSource(fragmentPreamble(chain.parameters, passSource->slangParameters, shaderBody, outputCount))
					.addSource(shaderBody);
			Program program;
			const std::string vertexShader = vertexSource.generate();
			program.handle = gl_CompileAndLink(vertexShader.c_str(), fragmentSource.generate().c_str());
			program.pass = pass;
			program.sourceUniform = glGetUniformLocation(program.handle, "Source");
			program.sourceSizeUniform = glGetUniformLocation(program.handle, "SourceSize");
			program.outputSizeUniform = glGetUniformLocation(program.handle, "OutputSize");
			program.frameCountUniform = glGetUniformLocation(program.handle, "FrameCount");
			for (const postprocess::ShaderParameter& parameter : chain.parameters)
				program.parameters.push_back({ glGetUniformLocation(program.handle, parameter.name.c_str()), parameter.name,
						parameter.type, parameter.values });
			for (const postprocess::SlangParameter& parameter : passSource->slangParameters) {
				if (std::any_of(program.parameters.begin(), program.parameters.end(), [&](const ParameterUniform& value) {
					return value.name == parameter.name;
				}))
					continue;
				program.parameters.push_back({ glGetUniformLocation(program.handle, parameter.name.c_str()), parameter.name,
						parameter.type, parameter.defaultValue.values });
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
				ResourceUniform binding { glGetUniformLocation(program.handle, resource.name.c_str()), input->kind,
						input->sourcePass, input->sourceOutput, input->historyOffset, input->resourceName, textureUnit++ };
				if (input->kind == postprocess::ShaderInputKind::Source) {
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
						postprocess::InvalidShaderPass, 0, 1, {}, textureUnit });
			}
			candidate.push_back(std::move(program));
		} catch (const RendererException& exception) {
			error = exception.what();
			deletePrograms(candidate);
			return false;
		}
	}
	return true;
}

bool GlShaderChain::activate(const postprocess::ShaderChain& chain, std::string& error)
{
	std::vector<Program> candidate;
	if (!compileCandidate(chain, candidate, error))
		return false;

	deletePrograms(programs);
	deleteLookupTextures();
	programs = std::move(candidate);
	historyDepth = 2;
	needsSourceHistory = false;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs)
			if (input.kind == postprocess::ShaderInputKind::History) {
				historyDepth = std::max(historyDepth, input.historyOffset + 1);
				needsSourceHistory = needsSourceHistory || input.sourcePass == postprocess::InvalidShaderPass;
			}
	targets.clear();
	sourceHistory.clear();
	lookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		lookupTextures.push_back({ texture });
	error.clear();
	return true;
}

void GlShaderChain::updateConfiguredChain()
{
	const std::string& requestedPath = config::PostProcessShader.get();
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPath == configuredPath && generation == configuredGeneration)
		return;
	configuredPath = requestedPath;
	configuredGeneration = generation;

	if (requestedPath.empty()) {
		deletePrograms(programs);
		deleteLookupTextures();
		targets.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return;
	}

	std::string error;
	postprocess::ShaderChain candidate;
	if (!postprocess::loadShaderChain(requestedPath, candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedPath.c_str(), error.c_str());
		return;
	}
	postprocess::applyShaderChainParameterValues(candidate, config::PostProcessShaderParameters.get());
	if (!activate(candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedPath.c_str(), error.c_str());
		return;
	}
	activePath = requestedPath;
	postprocess::reportShaderChainStatus(activePath, {}, candidate.parameters,
			candidate.techniques, candidate.selectedTechnique);
}

GlFramebuffer *GlShaderChain::render(GLuint sourceTexture, const postprocess::ShaderFrameInfo& frame)
{
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
	const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
	if (status.activePath == activePath && status.error.empty())
		for (Program& program : programs)
			for (ParameterUniform& parameter : program.parameters) {
				auto configured = std::find_if(status.parameters.begin(), status.parameters.end(), [&](const postprocess::ShaderParameter& value) {
					return value.name == parameter.name;
				});
				if (configured != status.parameters.end())
					std::copy_n(configured->values.begin(), configured->componentCount, parameter.values.begin());
			}
	if (targets.size() != programs.size())
		targets.resize(programs.size());
	const size_t currentTarget = frame.frameCount % historyDepth;
	if (needsSourceHistory) {
		if (sourceHistory.size() != historyDepth)
			sourceHistory.resize(historyDepth);
		for (std::unique_ptr<GlFramebuffer>& target : sourceHistory)
			if (target == nullptr || target->getWidth() != frame.sourceWidth || target->getHeight() != frame.sourceHeight) {
				target = std::make_unique<GlFramebuffer>(frame.sourceWidth, frame.sourceHeight, false, true);
				target->bind();
				glClearColor(0.f, 0.f, 0.f, 0.f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
		GlFramebuffer *historyTarget = sourceHistory[currentTarget].get();
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);
		historyTarget->bind();
		glViewport(0, 0, frame.sourceWidth, frame.sourceHeight);
		glcache.UseProgram(copyProgram);
		glActiveTexture(GL_TEXTURE0);
		glcache.BindTexture(GL_TEXTURE_2D, sourceTexture);
		if (copySourceUniform != -1)
			glUniform1i(copySourceUniform, 0);
		vertexArray.bind(vertexBuffer.get());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		GlVertexArray::unbind();
	}
	GLuint inputTexture = sourceTexture;
	GlFramebuffer *output = nullptr;
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		int outputWidth;
		int outputHeight;
		std::string error;
		if (!postprocess::resolveTargetSize(program.pass, passFrame, outputWidth, outputHeight, error))
			return nullptr;

		MrtTargetRing& targetRing = targets[index];
		if (targetRing.size() != historyDepth)
			targetRing.resize(historyDepth);
		for (std::unique_ptr<MrtTarget>& target : targetRing) {
			if (target == nullptr)
				target = std::make_unique<MrtTarget>();
			if (!ensureMrtTarget(*target, program.pass, outputWidth, outputHeight, error)) {
				postprocess::reportShaderChainStatus(activePath, error);
				return nullptr;
			}
		}
		MrtTarget *target = targetRing[currentTarget].get();

		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		if (program.pass.blendEnabled) {
			glcache.Enable(GL_BLEND);
			glcache.BlendFunc(glBlendFactor(program.pass.sourceBlend), glBlendFactor(program.pass.destinationBlend));
			glcache.BlendEquation(glBlendOperation(program.pass.blendOperation));
		}
		else {
			glcache.Disable(GL_BLEND);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
		glViewport(0, 0, outputWidth, outputHeight);
		glcache.UseProgram(program.handle);
		for (const ResourceUniform& resource : program.resources) {
			GLuint texture = inputTexture;
			const LookupTexture *lookup = nullptr;
			if ((resource.kind == postprocess::ShaderInputKind::PassOutput
						|| resource.kind == postprocess::ShaderInputKind::History
						|| resource.kind == postprocess::ShaderInputKind::Feedback)
					&& resource.sourcePass != postprocess::InvalidShaderPass
					&& (resource.sourcePass >= targets.size()
							|| resource.sourceOutput >= targets[resource.sourcePass][currentTarget]->outputs.size())) {
				error = "OpenGL shader graph references an invalid pass output.";
				postprocess::reportShaderChainStatus(activePath, error);
				return nullptr;
			}
			switch (resource.kind) {
			case postprocess::ShaderInputKind::Source:
				break;
			case postprocess::ShaderInputKind::Original:
				texture = sourceTexture;
				break;
			case postprocess::ShaderInputKind::PassOutput:
				texture = targets[resource.sourcePass][currentTarget]->outputs[resource.sourceOutput]->getTexture();
				break;
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback:
			{
				const uint32_t offset = resource.kind == postprocess::ShaderInputKind::Feedback ? 1 : resource.historyOffset;
				const size_t historyTarget = (currentTarget + historyDepth - offset % historyDepth) % historyDepth;
				texture = resource.sourcePass == postprocess::InvalidShaderPass
						? sourceHistory[historyTarget]->getTexture()
						: targets[resource.sourcePass][historyTarget]->outputs[resource.sourceOutput]->getTexture();
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture:
				const auto lookupIt = std::find_if(lookupTextures.begin(), lookupTextures.end(), [&](const LookupTexture& value) {
					return value.config.name == resource.resourceName;
				});
				if (lookupIt == lookupTextures.end())
					return nullptr;
				lookup = &*lookupIt;
				texture = lookup->texture;
				break;
			}
			glActiveTexture(GL_TEXTURE0 + resource.unit);
			glcache.BindTexture(GL_TEXTURE_2D, texture);
			if (resource.kind == postprocess::ShaderInputKind::Source && program.pass.useMipmaps)
				glGenerateMipmap(GL_TEXTURE_2D);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
					lookup != nullptr ? (lookup->config.useMipmaps ? GL_LINEAR_MIPMAP_LINEAR
							: lookup->config.linearFilter ? GL_LINEAR : GL_NEAREST)
						: program.pass.useMipmaps && resource.kind == postprocess::ShaderInputKind::Source
							? GL_LINEAR_MIPMAP_LINEAR : program.pass.linearFilter ? GL_LINEAR : GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
					lookup != nullptr ? (lookup->config.linearFilter ? GL_LINEAR : GL_NEAREST)
						: program.pass.linearFilter ? GL_LINEAR : GL_NEAREST);
			if (lookup != nullptr) {
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapMode(lookup->config.wrapMode));
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapMode(lookup->config.wrapMode));
			}
			if (resource.location != -1)
				glUniform1i(resource.location, static_cast<GLint>(resource.unit));
		}
		glActiveTexture(GL_TEXTURE0);
		if (program.sourceSizeUniform != -1)
			glUniform2f(program.sourceSizeUniform, static_cast<float>(passFrame.sourceWidth), static_cast<float>(passFrame.sourceHeight));
		if (program.outputSizeUniform != -1)
			glUniform2f(program.outputSizeUniform, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
		if (program.frameCountUniform != -1)
			glUniform1i(program.frameCountUniform, static_cast<GLint>(program.pass.frameCountModulo == 0
					? frame.frameCount : frame.frameCount % program.pass.frameCountModulo));
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
		for (uint32_t outputIndex = 0; outputIndex < target->outputs.size(); ++outputIndex) {
			const bool outputUsesMipmaps = program.pass.outputs.empty()
					? program.pass.useMipmaps : program.pass.outputs[outputIndex].useMipmaps;
			if (outputUsesMipmaps) {
				glActiveTexture(GL_TEXTURE0);
				glcache.BindTexture(GL_TEXTURE_2D, target->outputs[outputIndex]->getTexture());
				glGenerateMipmap(GL_TEXTURE_2D);
			}
		}

		inputTexture = target->outputs[0]->getTexture();
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
		output = target->outputs[0].get();
	}
	glCheck();
	return output;
}

void GlShaderChain::term()
{
	deletePrograms(programs);
	deleteLookupTextures();
	targets.clear();
	sourceHistory.clear();
	vertexBuffer.reset();
	if (copyProgram != 0)
		glcache.DeleteProgram(copyProgram);
	copyProgram = 0;
	copySourceUniform = -1;
	vertexArray.term();
	configuredPath.clear();
	configuredGeneration = 0;
	activePath.clear();
	postprocess::reportShaderChainStatus({}, {});
}
