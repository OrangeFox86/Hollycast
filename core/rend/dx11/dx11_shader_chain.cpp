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

} // namespace
void Dx11ShaderChain::init(const ComPtr<ID3D11Device>& device, const ComPtr<ID3D11DeviceContext>& deviceContext, Quad *quad)
{
	this->device = device;
	this->deviceContext = deviceContext;
	this->quad = quad;
}

bool Dx11ShaderChain::ensurePassSamplers(std::string& error)
{
	if (linearSampler && nearestSampler && mipLinearSampler && mipNearestSampler)
		return true;
	const auto create = [&](D3D11_FILTER filter, float maxLod, ComPtr<ID3D11SamplerState>& sampler) {
		D3D11_SAMPLER_DESC description {};
		description.Filter = filter;
		description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		description.ComparisonFunc = D3D11_COMPARISON_NEVER;
		description.MinLOD = 0.f;
		description.MaxLOD = maxLod;
		return SUCCEEDED(device->CreateSamplerState(&description, &sampler.get()));
	};
	if (!create(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, 0.f, linearSampler)
			|| !create(D3D11_FILTER_MIN_MAG_MIP_POINT, 0.f, nearestSampler)
			|| !create(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FLOAT32_MAX, mipLinearSampler)
			|| !create(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FLOAT32_MAX, mipNearestSampler)) {
		linearSampler.reset();
		nearestSampler.reset();
		mipLinearSampler.reset();
		mipNearestSampler.reset();
		error = "DirectX 11 could not create post-processing sampler states.";
		return false;
	}
	return true;
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
				if (parameter.layerIndex != pass.layerIndex)
					continue;
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
		if (!postprocess::translateGlslGraphFragmentToHlsl(*passSource, typedParameters, inputs,
				outputCount, hlslSource, error))
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
		program.duckStationGlsl = passSource->duckStationGlsl;
		program.parameters = std::move(typedParameters);
		for (const postprocess::SlangParameter& parameter : program.parameters) {
			const auto configured = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const postprocess::ShaderParameter& value) {
				return value.name == parameter.name && value.layerIndex == pass.layerIndex;
			});
			program.parameterIndices.push_back(configured == chain.parameters.end() ? postprocess::InvalidShaderPass
					: static_cast<uint32_t>(std::distance(chain.parameters.begin(), configured)));
		}
		program.outputFormats.reserve(outputCount);
		for (uint32_t output = 0; output < outputCount; ++output) {
			postprocess::ShaderPass outputPass = pass;
			if (!pass.outputs.empty()) {
				outputPass.targetFormat = pass.outputs[output].targetFormat;
				outputPass.targetFormatName = pass.outputs[output].targetFormatName;
			}
			DXGI_FORMAT targetFormat;
			if (!resolveTargetFormat(outputPass, targetFormat, error))
				return false;
			program.outputFormats.push_back(targetFormat);
		}
		for (size_t slot = 0; slot < inputs.size(); ++slot) {
			uint32_t lookupIndex = postprocess::InvalidShaderPass;
			if (inputs[slot].kind == postprocess::ShaderInputKind::LookupTexture) {
				const auto lookup = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const postprocess::ShaderPresetTexture& texture) {
					return texture.name == inputs[slot].resourceName;
				});
				if (lookup == chain.textures.end()) {
					error = "DirectX 11 lookup texture '" + inputs[slot].resourceName + "' was not resolved.";
					return false;
				}
				lookupIndex = static_cast<uint32_t>(std::distance(chain.textures.begin(), lookup));
			}
			ResourceBinding binding;
			binding.input = inputs[slot];
			binding.slot = static_cast<UINT>(slot);
			binding.lookupIndex = lookupIndex;
			const auto samplerConfig = std::find_if(pass.samplers.begin(), pass.samplers.end(), [&](const auto& sampler) {
				return sampler.bindingName == inputs[slot].bindingName;
			});
			if (samplerConfig != pass.samplers.end() && inputs[slot].kind != postprocess::ShaderInputKind::LookupTexture) {
				D3D11_SAMPLER_DESC description {};
				description.Filter = samplerConfig->linearFilter
						? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
				description.AddressU = textureAddressMode(samplerConfig->wrapMode);
				description.AddressV = description.AddressW = description.AddressU;
				description.ComparisonFunc = D3D11_COMPARISON_NEVER;
				description.MaxLOD = 0.f;
				if (FAILED(device->CreateSamplerState(&description, &binding.sampler.get()))) {
					error = "DirectX 11 could not create ReShade sampler '" + samplerConfig->bindingName + "'.";
					return false;
				}
				description.Filter = samplerConfig->linearFilter
						? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
				description.MaxLOD = D3D11_FLOAT32_MAX;
				if (FAILED(device->CreateSamplerState(&description, &binding.mipSampler.get()))) {
					error = "DirectX 11 could not create mipmapped ReShade sampler '" + samplerConfig->bindingName + "'.";
					return false;
				}
				binding.useMipmaps = samplerConfig->useMipmaps;
			}
			program.resources.push_back(std::move(binding));
		}
		program.resourceViews.resize(program.resources.size());
		program.samplers.resize(program.resources.size());
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
			if (!postprocess::translateGlslGraphVertexToHlsl(vertexSource, program.parameters, inputs,
					hlslVertex, error))
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
	const std::vector<std::string> requestedPaths = postprocess::configuredShaderPaths();
	const std::string requestedLabel = postprocess::shaderSelectionLabel(requestedPaths);
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPaths == configuredPaths && generation == configuredGeneration)
		return !programs.empty();
	configuredPaths = requestedPaths;
	configuredGeneration = generation;
	if (requestedPaths.empty()) {
		programs.clear();
		parameters.clear();
		targets.clear();
		sourceHistory.clear();
		lookupTextures.clear();
		displayInput = {};
		mipmapSource = {};
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return false;
	}
	std::string error;
	postprocess::ShaderChain chain;
	if (!postprocess::loadShaderChain(requestedPaths, chain, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	postprocess::applyShaderChainParameterValues(chain, config::PostProcessShaderParameters.get());
	if (!ensurePassSamplers(error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	std::vector<Program> candidate;
	if (!compileCandidate(chain, candidate, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	std::vector<LookupTexture> candidateLookupTextures;
	candidateLookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		candidateLookupTextures.push_back({ texture });
	if (!ensureLookupTextures(candidateLookupTextures, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	programs = std::move(candidate);
	parameters = std::move(chain.parameters);
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
	sourceHistory.clear();
	lookupTextures = std::move(candidateLookupTextures);
	activePath = requestedLabel;
	postprocess::reportShaderChainStatus(activePath, {}, parameters,
			chain.techniques, chain.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
	return true;
}

bool Dx11ShaderChain::ensureLookupTextures(std::string& error)
{
	return ensureLookupTextures(lookupTextures, error);
}

bool Dx11ShaderChain::ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error)
{
	uint64_t lookupBytes = 0;
	for (LookupTexture& lookup : textures) {
		if (lookup.view)
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
	target.initialized = false;
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

void Dx11ShaderChain::unbindPixelShaderSource(UINT count) const
{
	std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> nullViews {};
	count = std::min(count, static_cast<UINT>(nullViews.size()));
	deviceContext->PSSetShaderResources(0, count, nullViews.data());
	deviceContext->VSSetShaderResources(0, count, nullViews.data());
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

bool Dx11ShaderChain::render(ComPtr<ID3D11ShaderResourceView>& sourceView, const postprocess::ShaderFrameInfo& frame,
		ComPtr<ID3D11ShaderResourceView>& outputView, bool& presentationTransformApplied)
{
	static_assert(sizeof(ShaderFrameUniforms) <= Quad::ConstantBufferBytes,
			"DX11 post-processing uniforms must fit the shared quad constant buffer");
	presentationTransformApplied = false;
	if (!updateConfiguredChain() || !sourceView || frame.sourceWidth <= 0 || frame.sourceHeight <= 0)
		return false;
	std::string lookupError;
	if (!ensureLookupTextures(lookupError)) {
		postprocess::reportShaderChainStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "%s", lookupError.c_str());
		return false;
	}
	// External renderer state is unknown at chain entry. Clear it once; later
	// passes only unbind the slots that HollyCast itself populated.
	unbindPixelShaderSource(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
	postprocess::ShaderFrameInfo passFrame = frame;
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty())
			parameters = status.parameters;
	}
	ComPtr<ID3D11ShaderResourceView> inputView = sourceView;
	ComPtr<ID3D11ShaderResourceView> originalView = sourceView;
	bool inputHasMipChain = false;
	bool displaySpaceReached = false;
	int originalWidth = passFrame.sourceWidth;
	int originalHeight = passFrame.sourceHeight;
	auto composeDisplayInput = [&]() {
		if (displaySpaceReached)
			return true;
		displaySpaceReached = true;
		if (!postprocess::shouldComposeDisplayInput(frame, false))
			return true;
		if (!ensureTarget(displayInput, frame.viewportWidth, frame.viewportHeight, DXGI_FORMAT_B8G8R8A8_UNORM, false))
			return false;
		D3D11_VIEWPORT viewport {};
		viewport.Width = static_cast<float>(frame.viewportWidth);
		viewport.Height = static_cast<float>(frame.viewportHeight);
		viewport.MinDepth = 0.f;
		viewport.MaxDepth = 1.f;
		unbindPixelShaderSource();
		deviceContext->RSSetViewports(1, &viewport);
		deviceContext->OMSetRenderTargets(1, &displayInput.view.get(), nullptr);
		deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		quad->draw(inputView, DX11Context::Instance()->getSamplers().getSampler(config::LinearInterpolation),
				nullptr, -1.f, -1.f, 2.f, 2.f, frame.rotationPending);
		unbindPixelShaderSource();
		inputView = displayInput.shaderView;
		originalView = inputView;
		inputHasMipChain = false;
		passFrame.sourceWidth = frame.viewportWidth;
		passFrame.sourceHeight = frame.viewportHeight;
		originalWidth = passFrame.sourceWidth;
		originalHeight = passFrame.sourceHeight;
		presentationTransformApplied = true;
		return true;
	};
	if (programs.front().pass.composeDisplayInput && !composeDisplayInput())
		return false;
	if (targets.size() != programs.size())
		targets.resize(programs.size());
	postprocess::ShaderFrameInfo sizeFrame = frame;
	bool sizeDisplaySpaceReached = false;
	uint64_t targetBytes = 0;
	std::string targetBudgetError;
	if (std::any_of(programs.begin(), programs.end(), [](const Program& program) {
			return program.pass.composeDisplayInput;
		}) && postprocess::shouldComposeDisplayInput(frame, false)
			&& !postprocess::accumulateShaderTargetBytes(frame.viewportWidth, frame.viewportHeight,
					postprocess::ShaderTargetFormat::Default, {}, false, 1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderChainStatus(activePath, targetBudgetError);
		WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
		return false;
	}
	if (programs.front().pass.useMipmaps
			&& !postprocess::accumulateShaderTargetBytes(passFrame.sourceWidth, passFrame.sourceHeight,
					postprocess::ShaderTargetFormat::Default, {}, true, 1, targetBytes, targetBudgetError)) {
		postprocess::reportShaderChainStatus(activePath, targetBudgetError);
		WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
		return false;
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		if (program.pass.composeDisplayInput && !sizeDisplaySpaceReached) {
			sizeDisplaySpaceReached = true;
			if (postprocess::shouldComposeDisplayInput(frame, false)) {
				sizeFrame.sourceWidth = frame.viewportWidth;
				sizeFrame.sourceHeight = frame.viewportHeight;
			}
		}
		std::string error;
		if (!postprocess::resolveTargetSize(program.pass, sizeFrame,
				program.targetWidth, program.targetHeight, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "%s", error.c_str());
			return false;
		}
		sizeFrame.sourceWidth = program.targetWidth;
		sizeFrame.sourceHeight = program.targetHeight;
		const uint32_t outputCount = program.pass.outputs.empty()
				? 1u : static_cast<uint32_t>(program.pass.outputs.size());
		for (uint32_t output = 0; output < outputCount; ++output) {
			const postprocess::ShaderPassOutput config = program.pass.outputs.empty()
					? postprocess::ShaderPassOutput {} : program.pass.outputs[output];
			const postprocess::ShaderTargetFormat format = program.pass.outputs.empty()
					? program.pass.targetFormat : config.targetFormat;
			const std::string& formatName = program.pass.outputs.empty()
					? program.pass.targetFormatName : config.targetFormatName;
			if (!postprocess::accumulateShaderTargetBytes(program.targetWidth, program.targetHeight,
					format, formatName, program.outputNeedsMipmaps[output], program.historyDepth,
					targetBytes, targetBudgetError)) {
				postprocess::reportShaderChainStatus(activePath, targetBudgetError);
				WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
				return false;
			}
		}
		PassTargets& passTargets = targets[index];
		if (passTargets.size() != outputCount)
			passTargets.resize(outputCount);
		for (uint32_t output = 0; output < outputCount; ++output) {
			TargetRing& targetRing = passTargets[output];
			if (targetRing.size() != program.historyDepth)
				targetRing.resize(program.historyDepth);
			if (std::any_of(targetRing.begin(), targetRing.end(), [&](Target& target) {
				return !ensureTarget(target, program.targetWidth, program.targetHeight,
						program.outputFormats[output], program.outputNeedsMipmaps[output]);
			})) {
				error = "DirectX 11 post-processing target creation failed.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
		}
	}
	if (needsSourceHistory) {
		if (!postprocess::accumulateShaderTargetBytes(passFrame.sourceWidth, passFrame.sourceHeight,
				postprocess::ShaderTargetFormat::Default, {}, false, sourceHistoryDepth,
				targetBytes, targetBudgetError)) {
			postprocess::reportShaderChainStatus(activePath, targetBudgetError);
			WARN_LOG(RENDERER, "%s", targetBudgetError.c_str());
			return false;
		}
		if (sourceHistory.size() != sourceHistoryDepth)
			sourceHistory.resize(sourceHistoryDepth);
		if (std::any_of(sourceHistory.begin(), sourceHistory.end(), [&](Target& target) {
			return !ensureTarget(target, passFrame.sourceWidth, passFrame.sourceHeight, DXGI_FORMAT_B8G8R8A8_UNORM, false);
		})) {
			const std::string error = "DirectX 11 post-processing source history target creation failed.";
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "%s", error.c_str());
			return false;
		}
		D3D11_VIEWPORT historyViewport {};
		historyViewport.Width = static_cast<float>(passFrame.sourceWidth);
		historyViewport.Height = static_cast<float>(passFrame.sourceHeight);
		historyViewport.MinDepth = 0.f;
		historyViewport.MaxDepth = 1.f;
		unbindPixelShaderSource();
		deviceContext->RSSetViewports(1, &historyViewport);
		const size_t currentSourceTarget = frame.frameCount % sourceHistoryDepth;
		deviceContext->OMSetRenderTargets(1, &sourceHistory[currentSourceTarget].view.get(), nullptr);
		deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		quad->draw(originalView, DX11Context::Instance()->getSamplers().getSampler(false));
		unbindPixelShaderSource();
		sourceHistory[currentSourceTarget].renderedFrame = frame.frameCount;
		sourceHistory[currentSourceTarget].initialized = true;
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		if (program.pass.composeDisplayInput && !composeDisplayInput())
			return false;
		const size_t currentTarget = frame.frameCount % program.historyDepth;
		const int outputWidth = program.targetWidth;
		const int outputHeight = program.targetHeight;
		std::string error;
		const uint32_t outputCount = program.pass.outputs.empty() ? 1u : static_cast<uint32_t>(program.pass.outputs.size());
		PassTargets& passTargets = targets[index];
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
		if (program.pass.blendEnabled) {
			// ReShade blends against the current backbuffer. Seed HollyCast's
			// private target before enabling the pass blend state.
			deviceContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			quad->draw(inputView, DX11Context::Instance()->getSamplers().getSampler(program.pass.linearFilter));
			unbindPixelShaderSource();
			deviceContext->OMSetRenderTargets(outputCount, renderTargets.data(), nullptr);
		}
		deviceContext->OMSetBlendState(program.blendState, nullptr, 0xffffffff);
		ShaderFrameUniforms uniforms {};
		uniforms.sourceSize[0] = static_cast<float>(passFrame.sourceWidth);
		uniforms.sourceSize[1] = static_cast<float>(passFrame.sourceHeight);
		uniforms.outputSize[0] = static_cast<float>(outputWidth);
		uniforms.outputSize[1] = static_cast<float>(outputHeight);
		uniforms.frameCount = static_cast<int>(program.pass.frameCountModulo == 0 ? frame.frameCount : frame.frameCount % program.pass.frameCountModulo);
		uniforms.rotation = frame.rotationPending ? 1 : 0;
		uniforms.originalSize[0] = static_cast<float>(program.duckStationGlsl ? frame.sourceWidth : originalWidth);
		uniforms.originalSize[1] = static_cast<float>(program.duckStationGlsl ? frame.sourceHeight : originalHeight);
		uint32_t parameterOffset = 0;
		for (size_t parameterIndex = 0; parameterIndex < program.parameters.size(); ++parameterIndex) {
			const postprocess::SlangParameter& parameter = program.parameters[parameterIndex];
			const uint32_t components = parameterComponentCount(parameter.type);
			if (parameterOffset + components > std::size(uniforms.parameters)) {
				error = "DirectX 11 shader parameter storage overflow.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			std::copy_n(parameter.defaultValue.values.begin(), components, uniforms.parameters + parameterOffset);
			const uint32_t configuredIndex = program.parameterIndices[parameterIndex];
			if (configuredIndex < parameters.size())
				std::copy_n(parameters[configuredIndex].values.begin(), components, uniforms.parameters + parameterOffset);
			parameterOffset += components;
		}
		for (size_t resourceIndex = 0; resourceIndex < program.resources.size(); ++resourceIndex) {
			const ResourceBinding& binding = program.resources[resourceIndex];
			ComPtr<ID3D11ShaderResourceView> view;
			ComPtr<ID3D11SamplerState> sampler;
			bool resourceHasMipmaps = false;
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
				resourceHasMipmaps = inputHasMipChain;
				break;
			case postprocess::ShaderInputKind::Original:
				view = originalView;
				break;
			case postprocess::ShaderInputKind::PassOutput: {
				const uint32_t sourceDepth = programs[binding.input.sourcePass].historyDepth;
				const Target& sourceTarget = targets[binding.input.sourcePass][binding.input.sourceOutput]
						[frame.frameCount % sourceDepth];
				if (!sourceTarget.initialized || sourceTarget.renderedFrame != frame.frameCount) {
					error = "DirectX 11 shader pass input was not rendered for the current frame.";
					postprocess::reportShaderChainStatus(activePath, error);
					WARN_LOG(RENDERER, "%s", error.c_str());
					return false;
				}
				view = sourceTarget.shaderView;
				resourceHasMipmaps = programs[binding.input.sourcePass].outputNeedsMipmaps[binding.input.sourceOutput];
				break;
			}
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback:
			{
				const uint32_t offset = binding.input.kind == postprocess::ShaderInputKind::Feedback ? 1 : binding.input.historyOffset;
				if (frame.frameCount < offset) {
					view = originalView;
					break;
				}
				const uint32_t requestedFrame = frame.frameCount - offset;
				if (binding.input.sourcePass == postprocess::InvalidShaderPass) {
					const Target& history = sourceHistory[requestedFrame % sourceHistoryDepth];
					view = history.initialized && history.renderedFrame == requestedFrame
							? history.shaderView : originalView;
					resourceHasMipmaps = false;
				}
				else {
					const uint32_t sourceDepth = programs[binding.input.sourcePass].historyDepth;
					const Target& history = targets[binding.input.sourcePass][binding.input.sourceOutput]
							[requestedFrame % sourceDepth];
					view = history.initialized && history.renderedFrame == requestedFrame
							? history.shaderView : originalView;
				}
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture:
				if (binding.lookupIndex < lookupTextures.size()) {
					view = lookupTextures[binding.lookupIndex].view;
					sampler = lookupTextures[binding.lookupIndex].sampler;
				}
				break;
			}
			if (!view) {
				error = "DirectX 11 shader resource '" + binding.input.bindingName + "' is unavailable.";
				postprocess::reportShaderChainStatus(activePath, error);
				WARN_LOG(RENDERER, "%s", error.c_str());
				return false;
			}
			program.resourceViews[resourceIndex] = view;
			if (!sampler && binding.sampler)
				sampler = binding.useMipmaps && resourceHasMipmaps ? binding.mipSampler : binding.sampler;
			program.samplers[resourceIndex] = sampler ? sampler
					: program.pass.linearFilter
							? (resourceHasMipmaps ? mipLinearSampler : linearSampler)
							: (resourceHasMipmaps ? mipNearestSampler : nearestSampler);
		}
		quad->drawWithShaders(program.resourceViews, program.samplers, program.vertexShader.get(), program.pixelShader, &uniforms, sizeof(uniforms));
		unbindPixelShaderSource(static_cast<UINT>(program.resourceViews.size()));
		for (uint32_t output = 0; output < outputCount; ++output) {
			if (program.outputNeedsMipmaps[output])
				deviceContext->GenerateMips(passTargets[output][currentTarget].shaderView);
			passTargets[output][currentTarget].renderedFrame = frame.frameCount;
			passTargets[output][currentTarget].initialized = true;
		}
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
	displayInput = {};
	mipmapSource = {};
	sourceHistory.clear();
	programs.clear();
	lookupTextures.clear();
	parameters.clear();
	linearSampler.reset();
	nearestSampler.reset();
	mipLinearSampler.reset();
	mipNearestSampler.reset();
	configuredPaths.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	activePath.clear();
	sourceHistoryDepth = 1;
	needsSourceHistory = false;
	quad = nullptr;
	deviceContext.reset();
	device.reset();
	postprocess::reportShaderChainStatus({}, {});
}
