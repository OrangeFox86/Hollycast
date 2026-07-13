/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "dx11_shader_chain.h"

#include "cfg/option.h"
#include "dx11_quad.h"
#include "dx11context.h"
#include "log/LogManager.h"
#include "oslib/storage.h"
#include "stbi.h"

#include <algorithm>
#include <array>
#include <regex>
#include <sstream>

namespace {

bool isSampler2D(postprocess::SlangResourceType type)
{
	return type == postprocess::SlangResourceType::Sampler2D || type == postprocess::SlangResourceType::Texture2D;
}

uint32_t parameterComponentCount(postprocess::SlangParameterType type)
{
	switch (type) {
	case postprocess::SlangParameterType::Float2: return 2;
	case postprocess::SlangParameterType::Float3: return 3;
	case postprocess::SlangParameterType::Float4: return 4;
	case postprocess::SlangParameterType::Float2x2: return 4;
	case postprocess::SlangParameterType::Float3x3: return 9;
	case postprocess::SlangParameterType::Float4x4: return 16;
	default: return 1;
	}
}

D3D11_TEXTURE_ADDRESS_MODE textureAddressMode(postprocess::ShaderWrapMode mode)
{
	switch (mode) {
	case postprocess::ShaderWrapMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
	case postprocess::ShaderWrapMode::MirroredRepeat: return D3D11_TEXTURE_ADDRESS_MIRROR;
	case postprocess::ShaderWrapMode::ClampToBorder: return D3D11_TEXTURE_ADDRESS_BORDER;
	case postprocess::ShaderWrapMode::ClampToEdge: return D3D11_TEXTURE_ADDRESS_CLAMP;
	}
	return D3D11_TEXTURE_ADDRESS_CLAMP;
}

D3D11_BLEND blendFactor(postprocess::ShaderBlendFactor factor)
{
	switch (factor) {
	case postprocess::ShaderBlendFactor::Zero: return D3D11_BLEND_ZERO;
	case postprocess::ShaderBlendFactor::One: return D3D11_BLEND_ONE;
	case postprocess::ShaderBlendFactor::SourceColor: return D3D11_BLEND_SRC_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusSourceColor: return D3D11_BLEND_INV_SRC_COLOR;
	case postprocess::ShaderBlendFactor::DestinationColor: return D3D11_BLEND_DEST_COLOR;
	case postprocess::ShaderBlendFactor::OneMinusDestinationColor: return D3D11_BLEND_INV_DEST_COLOR;
	case postprocess::ShaderBlendFactor::SourceAlpha: return D3D11_BLEND_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusSourceAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
	case postprocess::ShaderBlendFactor::DestinationAlpha: return D3D11_BLEND_DEST_ALPHA;
	case postprocess::ShaderBlendFactor::OneMinusDestinationAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
	}
	return D3D11_BLEND_ONE;
}

D3D11_BLEND_OP blendOperation(postprocess::ShaderBlendOperation operation)
{
	switch (operation) {
	case postprocess::ShaderBlendOperation::Add: return D3D11_BLEND_OP_ADD;
	case postprocess::ShaderBlendOperation::Subtract: return D3D11_BLEND_OP_SUBTRACT;
	case postprocess::ShaderBlendOperation::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
	case postprocess::ShaderBlendOperation::Minimum: return D3D11_BLEND_OP_MIN;
	case postprocess::ShaderBlendOperation::Maximum: return D3D11_BLEND_OP_MAX;
	}
	return D3D11_BLEND_OP_ADD;
}

bool resolveTargetFormat(const postprocess::ShaderPass& pass, DXGI_FORMAT& format, std::string& error)
{
	if (pass.targetFormatName.empty()) {
		format = pass.targetFormat == postprocess::ShaderTargetFormat::Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
				: pass.targetFormat == postprocess::ShaderTargetFormat::Srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
				: DXGI_FORMAT_B8G8R8A8_UNORM;
		return true;
	}
	if (pass.targetFormatName == "R8G8B8A8_UNORM") format = DXGI_FORMAT_R8G8B8A8_UNORM;
	else if (pass.targetFormatName == "R8G8B8A8_SRGB") format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	else if (pass.targetFormatName == "R16G16B16A16_SFLOAT" || pass.targetFormatName == "RGBA16F") format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	else if (pass.targetFormatName == "R16G16B16A16_UNORM") format = DXGI_FORMAT_R16G16B16A16_UNORM;
	else if (pass.targetFormatName == "R32G32B32A32_SFLOAT") format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	else {
		error = "DirectX 11 does not support post-processing target format '" + pass.targetFormatName + "'.";
		return false;
	}
	return true;
}

void removeResourceDeclarations(std::string& source, const std::vector<postprocess::ShaderPassInput>& inputs,
		const std::vector<postprocess::SlangParameter>& parameters)
{
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);) {
		const bool resourceDeclaration = std::any_of(inputs.begin(), inputs.end(), [&](const postprocess::ShaderPassInput& input) {
			const std::regex pattern("^\\s*uniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?(?:sampler2D|texture2D)\\s+"
					+ input.bindingName + R"(\s*;\s*$)", std::regex::icase);
			return std::regex_match(line, pattern);
		});
		const bool parameterDeclaration = std::any_of(parameters.begin(), parameters.end(), [&](const postprocess::SlangParameter& parameter) {
			const std::regex pattern("^\\s*uniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?(?:float|int|bool|vec[2-4]|mat[2-4])\\s+"
					+ parameter.name + R"(\s*;\s*$)", std::regex::icase);
			return std::regex_match(line, pattern);
		});
		if (!resourceDeclaration && !parameterDeclaration)
			cleaned += line + '\n';
	}
	source = std::move(cleaned);
}

bool translateGraphFragment(const postprocess::ShaderPassSource& source,
		const std::vector<postprocess::SlangParameter>& parameters, const std::vector<postprocess::ShaderPassInput>& inputs,
		uint32_t outputCount, std::string& hlslSource, std::string& error)
{
	std::string glsl = source.fragment;
	removeResourceDeclarations(glsl, inputs, parameters);
	std::vector<std::string> sampledBindings;
	const std::regex textureCall(R"(\btexture\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)");
	for (std::sregex_iterator it(glsl.begin(), glsl.end(), textureCall), end; it != end; ++it) {
		const std::string name = (*it)[1].str();
		if (std::none_of(inputs.begin(), inputs.end(), [&](const postprocess::ShaderPassInput& input) { return input.bindingName == name; })) {
			error = "DirectX 11 shader samples undeclared resource '" + name + "'.";
			return false;
		}
		sampledBindings.push_back(name);
	}
	if (!sampledBindings.empty()) {
		std::string transformed;
		size_t offset = 0;
		for (std::sregex_iterator it(glsl.begin(), glsl.end(), textureCall), end; it != end; ++it) {
			const size_t position = static_cast<size_t>((*it).position());
			transformed.append(glsl, offset, position - offset);
			transformed += "texture(Source,";
			offset = position + static_cast<size_t>((*it).length());
		}
		transformed.append(glsl, offset, std::string::npos);
		glsl = std::move(transformed);
	}
	if (!postprocess::translateGlslFragmentToHlsl(glsl, parameters, hlslSource, error, outputCount))
		return false;

	const std::string marker = "SamplerState SourceSampler : register(s0);\n";
	const size_t markerPosition = hlslSource.find(marker);
	if (markerPosition == std::string::npos) {
		error = "DirectX 11 shader bridge did not emit its resource preamble.";
		return false;
	}
	std::string declarations;
	for (size_t slot = 1; slot < inputs.size(); ++slot)
		declarations += "Texture2D " + inputs[slot].bindingName + " : register(t" + std::to_string(slot) + ");\n"
				+ "SamplerState " + inputs[slot].bindingName + "Sampler : register(s" + std::to_string(slot) + ");\n";
	hlslSource.insert(markerPosition + marker.size(), declarations);

	size_t searchFrom = 0;
	for (const std::string& binding : sampledBindings) {
		const std::string sourceSample = "Source.Sample(SourceSampler,";
		const size_t position = hlslSource.find(sourceSample, searchFrom);
		if (position == std::string::npos) {
			error = "DirectX 11 shader bridge could not map a graph texture sample.";
			return false;
		}
		const std::string replacement = binding + ".Sample(" + binding + "Sampler,";
		hlslSource.replace(position, sourceSample.size(), replacement);
		searchFrom = position + replacement.size();
	}
	return true;
}

bool translateGraphVertex(const std::string& vertexSource,
		const std::vector<postprocess::SlangParameter>& parameters, const std::vector<postprocess::ShaderPassInput>& inputs,
		std::string& hlslSource, std::string& error)
{
	std::string glsl = vertexSource;
	removeResourceDeclarations(glsl, inputs, parameters);
	std::vector<std::string> sampledBindings;
	const std::regex textureCall(R"(\btexture\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)");
	for (std::sregex_iterator it(glsl.begin(), glsl.end(), textureCall), end; it != end; ++it) {
		const std::string name = (*it)[1].str();
		if (std::none_of(inputs.begin(), inputs.end(), [&](const postprocess::ShaderPassInput& input) { return input.bindingName == name; })) {
			error = "DirectX 11 vertex shader samples undeclared resource '" + name + "'.";
			return false;
		}
		sampledBindings.push_back(name);
	}
	if (!sampledBindings.empty()) {
		std::string transformed;
		size_t offset = 0;
		for (std::sregex_iterator it(glsl.begin(), glsl.end(), textureCall), end; it != end; ++it) {
			const size_t position = static_cast<size_t>((*it).position());
			transformed.append(glsl, offset, position - offset);
			transformed += "texture(Source,";
			offset = position + static_cast<size_t>((*it).length());
		}
		transformed.append(glsl, offset, std::string::npos);
		glsl = std::move(transformed);
	}
	if (!postprocess::translateGlslVertexToHlsl(glsl, parameters, hlslSource, error))
		return false;
	const std::string marker = "SamplerState SourceSampler : register(s0);\n";
	const size_t markerPosition = hlslSource.find(marker);
	if (markerPosition == std::string::npos) {
		error = "DirectX 11 vertex shader bridge did not emit its resource preamble.";
		return false;
	}
	std::string declarations;
	for (size_t slot = 1; slot < inputs.size(); ++slot)
		declarations += "Texture2D " + inputs[slot].bindingName + " : register(t" + std::to_string(slot) + ");\n"
				+ "SamplerState " + inputs[slot].bindingName + "Sampler : register(s" + std::to_string(slot) + ");\n";
	hlslSource.insert(markerPosition + marker.size(), declarations);
	size_t searchFrom = 0;
	for (const std::string& binding : sampledBindings) {
		const std::string sourceSample = "Source.Sample(SourceSampler,";
		const size_t position = hlslSource.find(sourceSample, searchFrom);
		if (position == std::string::npos) {
			error = "DirectX 11 vertex shader bridge could not map a graph texture sample.";
			return false;
		}
		const std::string replacement = binding + ".Sample(" + binding + "Sampler,";
		hlslSource.replace(position, sourceSample.size(), replacement);
		searchFrom = position + replacement.size();
	}
	return true;
}

} // namespace
void Dx11ShaderChain::init(const ComPtr<ID3D11Device>& device, const ComPtr<ID3D11DeviceContext>& deviceContext, Quad *quad)
{
	this->device = device;
	this->deviceContext = deviceContext;
	this->quad = quad;
}

bool Dx11ShaderChain::compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate,
		std::string& error) const
{
	const pD3DCompile compiler = DX11Context::Instance()->getCompiler();
	if (compiler == nullptr) {
		error = "DirectX 11 shader compiler is unavailable.";
		return false;
	}
	candidate.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		const postprocess::ShaderPass& pass = chain.passes[passIndex];
		postprocess::ShaderPassSource loadedSource;
		const postprocess::ShaderPassSource *passSource = passIndex < chain.sources.size()
				? &chain.sources[passIndex] : &loadedSource;
		if (passSource == &loadedSource && !postprocess::loadShaderPassSource(pass, loadedSource, error))
			return false;
		std::vector<postprocess::SlangParameter> typedParameters = passSource->slangParameters;
		if (typedParameters.empty())
			for (const postprocess::ShaderParameter& parameter : chain.parameters) {
				postprocess::SlangParameter typed;
				typed.name = parameter.name;
				typed.type = parameter.type;
				typed.defaultValue.values = parameter.values;
				typed.defaultValue.componentCount = parameter.componentCount;
				typedParameters.push_back(std::move(typed));
			}
		std::vector<postprocess::ShaderPassInput> inputs = pass.inputs;
		if (std::none_of(inputs.begin(), inputs.end(), [](const postprocess::ShaderPassInput& input) { return input.bindingName == "Source"; }))
			inputs.push_back({ "Source", postprocess::ShaderInputKind::Source, {}, postprocess::InvalidShaderPass });
		std::stable_sort(inputs.begin(), inputs.end(), [](const postprocess::ShaderPassInput& left, const postprocess::ShaderPassInput& right) {
			return left.bindingName == "Source" && right.bindingName != "Source";
		});
		if (inputs.front().bindingName != "Source" || inputs.size() > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
			error = inputs.front().bindingName != "Source"
					? "DirectX 11 could not assign Source to texture slot zero."
					: "DirectX 11 shader chains exceed the device resource-slot limit.";
			return false;
		}
		for (const postprocess::SlangResource& resource : passSource->slangResources)
			if (!isSampler2D(resource.type) || resource.arrayCount != 1) {
				error = "DirectX 11 supports only single sampler2D Slang resources; '" + resource.name + "' is unsupported.";
				return false;
			}
		std::string hlslSource;
		const uint32_t outputCount = pass.outputs.empty() ? 1u : static_cast<uint32_t>(pass.outputs.size());
		if (!translateGraphFragment(*passSource, typedParameters, inputs, outputCount, hlslSource, error))
			return false;
		ComPtr<ID3DBlob> shaderBlob;
		ComPtr<ID3DBlob> errorBlob;
		const HRESULT result = compiler(hlslSource.data(), hlslSource.size(), pass.sourcePath.c_str(), nullptr, nullptr,
				"main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shaderBlob.get(), &errorBlob.get());
		if (FAILED(result)) {
			error = errorBlob ? static_cast<const char *>(errorBlob->GetBufferPointer()) : "DirectX 11 HLSL compilation failed.";
			return false;
		}
		Program program;
		program.pass = pass;
		program.parameters = std::move(typedParameters);
		for (size_t slot = 0; slot < inputs.size(); ++slot)
			program.resources.push_back({ inputs[slot], static_cast<UINT>(slot) });
		if (FAILED(device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &program.pixelShader.get()))) {
			error = "DirectX 11 pixel shader creation failed.";
			return false;
		}
		if (pass.blendEnabled) {
			D3D11_BLEND_DESC blendDescription {};
			for (uint32_t output = 0; output < outputCount; ++output) {
				D3D11_RENDER_TARGET_BLEND_DESC& renderTarget = blendDescription.RenderTarget[output];
				renderTarget.BlendEnable = TRUE;
				renderTarget.SrcBlend = blendFactor(pass.sourceBlend);
				renderTarget.DestBlend = blendFactor(pass.destinationBlend);
				renderTarget.BlendOp = blendOperation(pass.blendOperation);
				renderTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
				renderTarget.DestBlendAlpha = D3D11_BLEND_ZERO;
				renderTarget.BlendOpAlpha = blendOperation(pass.blendOperation);
				renderTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			}
			if (FAILED(device->CreateBlendState(&blendDescription, &program.blendState.get()))) {
				error = "DirectX 11 post-processing blend-state creation failed.";
				return false;
			}
		}
		if (!passSource->vertex.empty()) {
			std::string vertexSource = passSource->vertex;
			std::string hlslVertex;
			if (!translateGraphVertex(vertexSource, program.parameters, inputs, hlslVertex, error))
				return false;
			ComPtr<ID3DBlob> vertexBlob;
			ComPtr<ID3DBlob> vertexErrorBlob;
			const HRESULT vertexResult = compiler(hlslVertex.data(), hlslVertex.size(), pass.sourcePath.c_str(), nullptr, nullptr,
					"main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexBlob.get(), &vertexErrorBlob.get());
			if (FAILED(vertexResult)) {
				error = vertexErrorBlob ? static_cast<const char *>(vertexErrorBlob->GetBufferPointer()) : "DirectX 11 vertex shader compilation failed.";
				return false;
			}
			if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &program.vertexShader.get()))) {
				error = "DirectX 11 vertex shader creation failed.";
				return false;
			}
		}
		candidate.push_back(std::move(program));
	}
	const std::vector<std::vector<bool>> mipmapRequirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	for (size_t passIndex = 0; passIndex < candidate.size(); ++passIndex)
		candidate[passIndex].outputNeedsMipmaps = mipmapRequirements[passIndex];
	return true;
}

bool Dx11ShaderChain::updateConfiguredChain()
{
	const std::string& requestedPath = config::PostProcessShader.get();
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPath == configuredPath && generation == configuredGeneration)
		return !programs.empty();
	configuredPath = requestedPath;
	configuredGeneration = generation;
	if (requestedPath.empty()) {
		programs.clear();
		parameters.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return false;
	}
	std::string error;
	postprocess::ShaderChain chain;
	if (!postprocess::loadShaderChain(requestedPath, chain, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedPath.c_str(), error.c_str());
		return !programs.empty();
	}
	postprocess::applyShaderChainParameterValues(chain, config::PostProcessShaderParameters.get());
	std::vector<Program> candidate;
	if (!compileCandidate(chain, candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedPath.c_str(), error.c_str());
		return !programs.empty();
	}
	programs = std::move(candidate);
	parameters = std::move(chain.parameters);
	historyDepth = 1;
	needsSourceHistory = false;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			if (input.kind == postprocess::ShaderInputKind::History) {
				historyDepth = std::max(historyDepth, input.historyOffset + 1);
				needsSourceHistory = needsSourceHistory || input.sourcePass == postprocess::InvalidShaderPass;
			}
			else if (input.kind == postprocess::ShaderInputKind::Feedback)
				historyDepth = std::max(historyDepth, 2u);
		}
	targets.clear();
	sourceHistory.clear();
	lookupTextures.clear();
	lookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		lookupTextures.push_back({ texture });
	activePath = requestedPath;
	postprocess::reportShaderChainStatus(activePath, {}, parameters,
			chain.techniques, chain.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
	return true;
}

bool Dx11ShaderChain::ensureLookupTextures(std::string& error)
{
	for (LookupTexture& lookup : lookupTextures) {
		if (lookup.view)
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
		D3D11_TEXTURE2D_DESC description {};
		description.Width = static_cast<UINT>(width);
		description.Height = static_cast<UINT>(height);
		description.MipLevels = lookup.config.useMipmaps ? 0 : 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE | (lookup.config.useMipmaps ? D3D11_BIND_RENDER_TARGET : 0);
		description.MiscFlags = lookup.config.useMipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
		const D3D11_SUBRESOURCE_DATA data { pixels, static_cast<UINT>(width * 4), 0 };
		ComPtr<ID3D11Texture2D> texture;
		const HRESULT result = device->CreateTexture2D(&description, lookup.config.useMipmaps ? nullptr : &data, &texture.get());
		if (SUCCEEDED(result) && lookup.config.useMipmaps)
			deviceContext->UpdateSubresource(texture, 0, nullptr, pixels, static_cast<UINT>(width * 4), 0);
		stbi_image_free(pixels);
		if (FAILED(result) || FAILED(device->CreateShaderResourceView(texture, nullptr, &lookup.view.get()))) {
			lookup.view.reset();
			error = "DirectX 11 could not create post-processing lookup texture: " + lookup.config.path;
			return false;
		}
		if (lookup.config.useMipmaps)
			deviceContext->GenerateMips(lookup.view);
		D3D11_SAMPLER_DESC sampler {};
		sampler.Filter = lookup.config.useMipmaps
				? (lookup.config.linearFilter ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR)
				: (lookup.config.linearFilter ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT);
		sampler.AddressU = textureAddressMode(lookup.config.wrapMode);
		sampler.AddressV = textureAddressMode(lookup.config.wrapMode);
		sampler.AddressW = textureAddressMode(lookup.config.wrapMode);
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampler, &lookup.sampler.get()))) {
			const std::string path = lookup.config.path;
			lookup = {};
			error = "DirectX 11 could not create post-processing lookup sampler: " + path;
			return false;
		}
	}
	return true;
}

bool Dx11ShaderChain::ensureTarget(Target& target, int width, int height, DXGI_FORMAT format, bool mipmapped)
{
	if (target.texture && target.width == width && target.height == height && target.format == format && target.mipmapped == mipmapped)
		return true;
	target.shaderView.reset();
	target.view.reset();
	target.texture.reset();
	D3D11_TEXTURE2D_DESC description {};
	description.Width = width;
	description.Height = height;
	description.MipLevels = mipmapped ? 0 : 1;
	description.ArraySize = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	description.MiscFlags = mipmapped ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
	D3D11_RENDER_TARGET_VIEW_DESC renderTargetDescription {};
	renderTargetDescription.Format = description.Format;
	renderTargetDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	renderTargetDescription.Texture2D.MipSlice = 0;
	D3D11_SHADER_RESOURCE_VIEW_DESC shaderResourceDescription {};
	shaderResourceDescription.Format = description.Format;
	shaderResourceDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shaderResourceDescription.Texture2D.MostDetailedMip = 0;
	shaderResourceDescription.Texture2D.MipLevels = mipmapped ? UINT(-1) : 1;
	if (FAILED(device->CreateTexture2D(&description, nullptr, &target.texture.get())) ||
			FAILED(device->CreateRenderTargetView(target.texture, &renderTargetDescription, &target.view.get())) ||
			FAILED(device->CreateShaderResourceView(target.texture, &shaderResourceDescription, &target.shaderView.get()))) {
		target.shaderView.reset();
		target.view.reset();
		target.texture.reset();
		target.width = 0;
		target.height = 0;
		target.format = DXGI_FORMAT_UNKNOWN;
		target.mipmapped = false;
		return false;
	}
	target.width = width;
	target.height = height;
	target.format = format;
	target.mipmapped = mipmapped;
	// The alternate target is sampled by history/feedback before its first render.
	// Define it instead of exposing driver-dependent texture contents on that frame.
	static constexpr float clearColor[] = { 0.f, 0.f, 0.f, 0.f };
	deviceContext->ClearRenderTargetView(target.view, clearColor);
	return true;
}

void Dx11ShaderChain::unbindPixelShaderSource() const
{
	std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> nullViews {};
	deviceContext->PSSetShaderResources(0, static_cast<UINT>(nullViews.size()), nullViews.data());
	deviceContext->VSSetShaderResources(0, static_cast<UINT>(nullViews.size()), nullViews.data());
}

bool Dx11ShaderChain::prepareMipmappedSource(ComPtr<ID3D11ShaderResourceView>& sourceView, int width, int height,
		ComPtr<ID3D11ShaderResourceView>& mipmappedView)
{
	if (!ensureTarget(mipmapSource, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, true))
		return false;

	// The emulator framebuffer has one mip level. Blit it into an internal,
	// mip-capable target before generation instead of issuing GenerateMips on a
	// resource that cannot legally provide a complete mip chain.
	D3D11_VIEWPORT viewport {};
	viewport.Width = static_cast<float>(width);
	viewport.Height = static_cast<float>(height);
	viewport.MinDepth = 0.f;
	viewport.MaxDepth = 1.f;
	unbindPixelShaderSource();
	deviceContext->RSSetViewports(1, &viewport);
	deviceContext->OMSetRenderTargets(1, &mipmapSource.view.get(), nullptr);
	deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	quad->draw(sourceView, DX11Context::Instance()->getSamplers().getSampler(false));
	unbindPixelShaderSource();
	deviceContext->GenerateMips(mipmapSource.shaderView);
	mipmappedView = mipmapSource.shaderView;
	return true;
}

bool Dx11ShaderChain::render(ComPtr<ID3D11ShaderResourceView>& sourceView, int width, int height, uint32_t frameCount,
		ComPtr<ID3D11ShaderResourceView>& outputView)
{
	if (!updateConfiguredChain() || !sourceView || width <= 0 || height <= 0)
		return false;
	std::string lookupError;
	if (!ensureLookupTextures(lookupError)) {
		postprocess::reportShaderChainStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "%s", lookupError.c_str());
		return false;
	}
	postprocess::ShaderFrameInfo passFrame { width, height, width, height, frameCount };
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty())
			parameters = status.parameters;
	}
	ComPtr<ID3D11ShaderResourceView> inputView = sourceView;
	bool inputHasMipChain = false;
	if (targets.size() != programs.size())
		targets.resize(programs.size());
	const size_t currentTarget = frameCount % historyDepth;
	if (needsSourceHistory) {
		if (sourceHistory.size() != historyDepth)
			sourceHistory.resize(historyDepth);
		if (std::any_of(sourceHistory.begin(), sourceHistory.end(), [&](Target& target) {
			return !ensureTarget(target, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, false);
		})) {
			const std::string error = "DirectX 11 post-processing source history target creation failed.";
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "%s", error.c_str());
			return false;
		}
		D3D11_VIEWPORT historyViewport {};
		historyViewport.Width = static_cast<float>(width);
		historyViewport.Height = static_cast<float>(height);
		historyViewport.MinDepth = 0.f;
		historyViewport.MaxDepth = 1.f;
		unbindPixelShaderSource();
		deviceContext->RSSetViewports(1, &historyViewport);
		deviceContext->OMSetRenderTargets(1, &sourceHistory[currentTarget].view.get(), nullptr);
		deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		quad->draw(sourceView, DX11Context::Instance()->getSamplers().getSampler(false));
		unbindPixelShaderSource();
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		int outputWidth;
		int outputHeight;
		std::string error;
		if (!postprocess::resolveTargetSize(program.pass, passFrame, outputWidth, outputHeight, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "%s", error.c_str());
			return false;
		}
		const uint32_t outputCount = program.pass.outputs.empty() ? 1u : static_cast<uint32_t>(program.pass.outputs.size());
		PassTargets& passTargets = targets[index];
		if (passTargets.size() != outputCount)
			passTargets.resize(outputCount);
		for (uint32_t output = 0; output < outputCount; ++output) {
			postprocess::ShaderPass outputPass = program.pass;
			if (!program.pass.outputs.empty()) {
				const postprocess::ShaderPassOutput& outputConfig = program.pass.outputs[output];
				outputPass.targetFormat = outputConfig.targetFormat;
				outputPass.targetFormatName = outputConfig.targetFormatName;
			}
			DXGI_FORMAT targetFormat;
			if (!resolveTargetFormat(outputPass, targetFormat, error)) {
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			TargetRing& targetRing = passTargets[output];
			if (targetRing.size() != historyDepth)
				targetRing.resize(historyDepth);
			if (std::any_of(targetRing.begin(), targetRing.end(), [&](Target& target) {
				return !ensureTarget(target, outputWidth, outputHeight, targetFormat, program.outputNeedsMipmaps[output]);
			})) {
				if (error.empty())
					error = "DirectX 11 post-processing target creation failed.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
		}
		Target& target = passTargets[0][currentTarget];
		if (program.pass.useMipmaps && !inputHasMipChain) {
			if (!prepareMipmappedSource(inputView, passFrame.sourceWidth, passFrame.sourceHeight, inputView)) {
				error = "DirectX 11 post-processing mip source creation failed.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			inputHasMipChain = true;
		}
		D3D11_VIEWPORT viewport {};
		viewport.Width = static_cast<float>(outputWidth);
		viewport.Height = static_cast<float>(outputHeight);
		viewport.MinDepth = 0.f;
		viewport.MaxDepth = 1.f;
		deviceContext->RSSetViewports(1, &viewport);
		unbindPixelShaderSource();
		std::array<ID3D11RenderTargetView *, 8> renderTargets {};
		for (uint32_t output = 0; output < outputCount; ++output)
			renderTargets[output] = passTargets[output][currentTarget].view.get();
		deviceContext->OMSetRenderTargets(outputCount, renderTargets.data(), nullptr);
		deviceContext->OMSetBlendState(program.blendState, nullptr, 0xffffffff);
		ShaderFrameUniforms uniforms {};
		uniforms.sourceSize[0] = static_cast<float>(passFrame.sourceWidth);
		uniforms.sourceSize[1] = static_cast<float>(passFrame.sourceHeight);
		uniforms.outputSize[0] = static_cast<float>(outputWidth);
		uniforms.outputSize[1] = static_cast<float>(outputHeight);
		uniforms.frameCount = static_cast<int>(program.pass.frameCountModulo == 0 ? frameCount : frameCount % program.pass.frameCountModulo);
		uint32_t parameterOffset = 0;
		for (const postprocess::SlangParameter& parameter : program.parameters) {
			const uint32_t components = parameterComponentCount(parameter.type);
			if (parameterOffset + components > std::size(uniforms.parameters)) {
				error = "DirectX 11 shader parameter storage overflow.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			std::copy_n(parameter.defaultValue.values.begin(), components, uniforms.parameters + parameterOffset);
			const auto configured = std::find_if(parameters.begin(), parameters.end(), [&](const postprocess::ShaderParameter& value) {
				return value.name == parameter.name;
			});
			if (configured != parameters.end())
				std::copy_n(configured->values.begin(), components, uniforms.parameters + parameterOffset);
			parameterOffset += components;
		}
		std::vector<ComPtr<ID3D11ShaderResourceView>> resourceViews;
		std::vector<ComPtr<ID3D11SamplerState>> samplers;
		resourceViews.reserve(program.resources.size());
		samplers.reserve(program.resources.size());
		for (const ResourceBinding& binding : program.resources) {
			ComPtr<ID3D11ShaderResourceView> view;
			ComPtr<ID3D11SamplerState> sampler;
			if ((binding.input.kind == postprocess::ShaderInputKind::PassOutput
						|| binding.input.kind == postprocess::ShaderInputKind::History
						|| binding.input.kind == postprocess::ShaderInputKind::Feedback)
					&& binding.input.sourcePass != postprocess::InvalidShaderPass
					&& (binding.input.sourcePass >= targets.size()
							|| binding.input.sourceOutput >= targets[binding.input.sourcePass].size())) {
				error = "DirectX 11 shader graph references an invalid pass output.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			switch (binding.input.kind) {
			case postprocess::ShaderInputKind::Source:
				view = inputView;
				break;
			case postprocess::ShaderInputKind::Original:
				view = sourceView;
				break;
			case postprocess::ShaderInputKind::PassOutput:
				view = targets[binding.input.sourcePass][binding.input.sourceOutput][currentTarget].shaderView;
				break;
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback:
			{
				const uint32_t offset = binding.input.kind == postprocess::ShaderInputKind::Feedback ? 1 : binding.input.historyOffset;
				const size_t historyTarget = (currentTarget + historyDepth - offset % historyDepth) % historyDepth;
				view = binding.input.sourcePass == postprocess::InvalidShaderPass
						? sourceHistory[historyTarget].shaderView : targets[binding.input.sourcePass][binding.input.sourceOutput][historyTarget].shaderView;
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture:
				if (const auto lookup = std::find_if(lookupTextures.begin(), lookupTextures.end(), [&](const LookupTexture& value) {
					return value.config.name == binding.input.resourceName;
				}); lookup != lookupTextures.end()) {
					view = lookup->view;
					sampler = lookup->sampler;
				}
				break;
			}
			if (!view) {
				error = "DirectX 11 shader resource '" + binding.input.bindingName + "' is unavailable.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			resourceViews.push_back(view);
			samplers.push_back(sampler ? sampler : DX11Context::Instance()->getSamplers().getSampler(program.pass.linearFilter));
		}
		quad->drawWithShaders(resourceViews, samplers, program.vertexShader.get(), program.pixelShader, &uniforms, sizeof(uniforms));
		unbindPixelShaderSource();
		for (uint32_t output = 0; output < outputCount; ++output)
			if (program.outputNeedsMipmaps[output])
				deviceContext->GenerateMips(passTargets[output][currentTarget].shaderView);
		inputView = target.shaderView;
		inputHasMipChain = program.outputNeedsMipmaps[0];
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
	}
	outputView = inputView;
	return true;
}

void Dx11ShaderChain::term()
{
	targets.clear();
	mipmapSource = {};
	sourceHistory.clear();
	programs.clear();
	lookupTextures.clear();
	parameters.clear();
	configuredPath.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	activePath.clear();
	historyDepth = 1;
	needsSourceHistory = false;
	quad = nullptr;
	deviceContext.reset();
	device.reset();
	postprocess::reportShaderChainStatus({}, {});
}
