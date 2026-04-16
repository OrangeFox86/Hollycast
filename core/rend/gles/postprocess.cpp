/*
	PowerVR2 buffer shader
    Authors: leilei

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
#include "postprocess.h"
#include "cfg/option.h"

#ifdef LIBRETRO
#include "quad.h"
#include <array>

PostProcessor postProcessor;

static const char* VertexShaderSource = R"(
in vec3 in_pos;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
}
)";

static const char* FragmentShaderSource = R"(
#define LUMBOOST 0

#if TARGET_GL == GLES2 || TARGET_GL == GLES3
precision highp float;
#endif

uniform int FrameCount;
uniform sampler2D Texture;
uniform vec2 videoShift;

// compatibility #defines
#define Source Texture
#define TextureSize textureSize(Texture, 0)
#define vTexCoord ((gl_FragCoord.xy + videoShift) / vec2(textureSize(Texture, 0)))

float dithertable[16] = float[](
	16.,4.,13.,1.,   
	8.,12.,5.,9.,
	14.,2.,15.,3.,
	6.,10.,7.,11.		
);

//#pragma parameter INTERLACED "PVR - Interlace smoothing" 1.00 0.00 1.00 1.0
//#pragma parameter VGASIGNAL "PVR - VGA signal loss" 0.00 0.00 1.00 1.0
//#pragma parameter LUMBOOST "PVR - Luminance gain" 0.35 0.00 1.00 0.01

#define LUM_R (76.0/255.0)
#define LUM_G (150.0/255.0)
#define LUM_B (28.0/255.0)

void main()
{
	vec2 texcoord = vTexCoord;
	texcoord.y = 1. - texcoord.y;
	vec2 texcoord2 = vTexCoord;
	texcoord2.y = 1. - texcoord2.y;
	texcoord2.x *= float(TextureSize.x);
	texcoord2.y *= float(TextureSize.y);
	vec4 color = texture(Source, texcoord);
	float fc = mod(float(FrameCount), 2.0);

#if INTERLACED == 1
	// Blend vertically for composite mode
	int taps = int(3);
	float tap = (2.666f/float(taps)) / float(min(TextureSize.y, 720));
	vec2 texcoord4  = vTexCoord;
	texcoord4.y = 1. - texcoord4.y;
	texcoord4.y -= tap * 2.f;
	int bl;
	vec4 ble = vec4(0.0);

	for (bl=0;bl<taps;bl++)
	{
		texcoord4.y += tap;
		ble.rgb += (texture(Source, texcoord4).rgb / float(taps+1));
	}
	color.rgb = (color.rgb / float(taps+1)) + ( ble.rgb );
#endif

#if LUMBOOST == 1
	// Some games use a luminance boost (JSR etc)
	color.rgb += (((color.r * LUM_R) + (color.g * LUM_G) + (color.b * LUM_B)) * LUMBOOST);
#endif

#if DITHERING == 1
	// Dither
	int ditdex = 	int(mod(texcoord2.x, 4.0)) * 4 + int(mod(texcoord2.y, 4.0)); 	
	int yeh = 0;
	float ohyes;
	vec4 how;

	for (yeh=ditdex; yeh<(ditdex+16); yeh++) 	ohyes =  ((((dithertable[yeh-15]) - 1.f) * 0.1));
	color.rb -= (ohyes / 128.);
	color.g -= (ohyes / 128.);
	{
		vec4 reduct;		// 16 bits per pixel (5-6-5)
		reduct.r = 32.;
		reduct.g = 64.;	
		reduct.b = 32.;
		how = color;
  		how = pow(how, vec4(1.0, 1.0, 1.0, 1.0));  	how *= reduct;  	how = floor(how);	how = how / reduct;  	how = pow(how, vec4(1.0, 1.0, 1.0, 1.0));
	}

	color.rb = how.rb;
	color.g = how.g;
#endif

#if VGASIGNAL == 1
	// There's a bit of a precision drop involved in the RGB565ening for VGA
	// I'm not sure why that is. it's exhibited on PVR1 and PVR3 hardware too
	if (mod(color.r*32, 2.0)>0) color.r -= 0.023;
	if (mod(color.g*64, 2.0)>0) color.g -= 0.01;
	if (mod(color.b*32, 2.0)>0) color.b -= 0.023;
#endif

	// RGB565 clamp

	color.rb = floor(color.rb * 32. + 0.5)/32.;
	color.g = floor(color.g * 64. + 0.5)/64.;

#if VGASIGNAL == 1
	// VGA Signal Loss, which probably is very wrong but i tried my best
	int taps = 32;
	float tap = 12.0/taps;
	vec2 texcoord4  = vTexCoord;
	texcoord4.y = 1. - texcoord4.y;
	texcoord4.x = texcoord4.x + (2.0/640.0);
	texcoord4.y = texcoord4.y;
	vec4 blur1 = texture(Source, texcoord4);
	int bl;
	vec4 ble = vec4(0.0);
	for (bl=0;bl<taps;bl++)
	{
		float e = 1;
		if (bl>=3)
			e=0.35;
		texcoord4.x -= (tap  / 640);
		ble.rgb += (texture(Source, texcoord4).rgb * e) / (taps/(bl+1));
	}

	color.rgb += ble.rgb * 0.015;

	//color.rb += (4.0/255.0);
	color.g += (9.0/255.0);
#endif

	FragColor = vec4(color);
} 
)";

class PostProcessShader
{
public:
	static void select(bool dither, bool interlaced, bool vga)
	{
		u32 key = ((int)dither << 2) | ((int)interlaced << 1) | (int)vga;
		if (shaders[key].program == 0)
			shaders[key].compile(dither, interlaced, vga);
		shaders[key].select();
	}
	static void term()
	{
		for (auto& shader : shaders)
		{
			if (shader.program != 0)
			{
				glDeleteProgram(shader.program);
				shader.program = 0;
			}
		}
	}

private:
	void compile(bool dither, bool interlaced, bool vga)
	{
		try
		{
			OpenGlSource vertexShader;
			vertexShader.addSource(VertexCompatShader)
					.addSource(VertexShaderSource);

			OpenGlSource fragmentShader;
			fragmentShader.addConstant("DITHERING", dither)
					.addConstant("INTERLACED", interlaced)
					.addConstant("VGASIGNAL", vga)
					.addSource(PixelCompatShader)
					.addSource(FragmentShaderSource);

			program = gl_CompileAndLink(vertexShader.generate().c_str(), fragmentShader.generate().c_str());

			//setup texture 0 as the input for the shader
			GLint gu = glGetUniformLocation(program, "Texture");
			if (gu != -1)
				glUniform1i(gu, 0);

			frameCountUniform = glGetUniformLocation(program, "FrameCount");
			videoShiftUniform = glGetUniformLocation(program, "videoShift");
		}
		catch (const std::exception& e)
		{
			program = 0;
			frameCountUniform = -1;
			videoShiftUniform = -1;
			WARN_LOG(RENDERER, "PostProcessor shader compile failed: %s", e.what());
		}
	}

	void select()
	{
		if (program == 0)
			return;
		glcache.UseProgram(program);
		glUniform1f(frameCountUniform, FrameCount);
		float shift[] = { -gl.ofbo.shiftX, gl.ofbo.shiftY };
		glUniform2fv(videoShiftUniform, 1, shift);
	}

	GLuint program = 0;
	GLint frameCountUniform = -1;
	GLint videoShiftUniform = -1;
	static std::array<PostProcessShader, 8> shaders;
};

std::array<PostProcessShader, 8> PostProcessShader::shaders;

void PostProcessor::VertexArray::defineVtxAttribs()
{
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void*)0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);
}

void PostProcessor::init(int width, int height)
{
	framebuffer = std::make_unique<GlFramebuffer>(width, height, true, true);

	float vertices[] = {
			-1,  1, 1,
			-1, -1, 1,
			 1,  1, 1,
			 1, -1, 1,
	};
	vertexBuffer = std::make_unique<GlBuffer>(GL_ARRAY_BUFFER, GL_STATIC_DRAW);
	vertexBuffer->update(vertices, sizeof(vertices));
	glCheck();
}

void PostProcessor::term()
{
	framebuffer.reset();
	vertexBuffer.reset();
	vertexArray.term();
	PostProcessShader::term();
	glCheck();
}


GLuint PostProcessor::getFramebuffer(int width, int height)
{
	if (framebuffer != nullptr
			&& (width != framebuffer->getWidth() || height != framebuffer->getHeight()))
		term();

	if (framebuffer == nullptr)
		init(width, height);

	return framebuffer->getFramebuffer();
}

void PostProcessor::render(GLuint output_fbo)
{
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Disable(GL_BLEND);

	if (!config::PowerVR2Filter)
	{
		// Just handle shifting and Y flipping
		if (gl.bogusBlitFramebuffer)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
			glViewport(0, 0, framebuffer->getWidth(), framebuffer->getHeight());
			glcache.ClearColor(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.f);
			glClear(GL_COLOR_BUFFER_BIT);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			static float vertices[20] = {
				-1.f, -1.f, 1.f, 0.f, 1.f,
				-1.f,  1.f, 1.f, 0.f, 0.f,
				 1.f, -1.f, 1.f, 1.f, 1.f,
				 1.f,  1.f, 1.f, 1.f, 0.f,
			};
			vertices[0] = vertices[5] = -1.f + gl.ofbo.shiftX * 2.f / framebuffer->getWidth();
			vertices[10] = vertices[15] = vertices[0] + 2;
			vertices[1] = vertices[11] = -1.f - gl.ofbo.shiftY * 2.f / framebuffer->getHeight();
			vertices[6] = vertices[16] = vertices[1] + 2;
			glcache.Disable(GL_BLEND);
			gl.quad->draw(framebuffer->getTexture(), false, false, vertices);
		}
		else
		{
#ifndef GLES2
			framebuffer->bind(GL_READ_FRAMEBUFFER);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, output_fbo);
			glcache.ClearColor(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.f);
			glClear(GL_COLOR_BUFFER_BIT);
			glBlitFramebuffer(-gl.ofbo.shiftX, -gl.ofbo.shiftY, framebuffer->getWidth() - gl.ofbo.shiftX, framebuffer->getHeight() - gl.ofbo.shiftY,
					0, framebuffer->getHeight(), framebuffer->getWidth(), 0,
					GL_COLOR_BUFFER_BIT, GL_NEAREST);
	    	glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
#endif
		}
		return;
	}

	if (gl.rendContext == nullptr)
		// Framebuffer render: no dithering
		PostProcessShader::select(false,
				SPG_CONTROL.interlace,
				FB_R_CTRL.vclk_div == 1 && SPG_CONTROL.interlace == 0);
	else
		PostProcessShader::select(gl.rendContext->fb_W_CTRL.fb_dither == 1 && gl.rendContext->fb_W_CTRL.fb_packmode <= 3 && !config::EmulateFramebuffer,
				SPG_CONTROL.interlace,
				FB_R_CTRL.vclk_div == 1 && SPG_CONTROL.interlace == 0);
	vertexArray.bind(vertexBuffer.get());

	glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);
	glActiveTexture(GL_TEXTURE0);
	glcache.BindTexture(GL_TEXTURE_2D, framebuffer->getTexture());

	glcache.ClearColor(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	GlVertexArray::unbind();
}
#endif // LIBRETRO

#ifndef LIBRETRO
#include "quad.h"
#include "rend/postfx/postfx.h"

#include <array>
#include <cmath>
#include <unordered_map>

namespace
{

constexpr size_t kMaxPostFxSamplers = 8;

static const char* PostFxVertexSource = R"(
in vec3 in_pos;
in vec2 in_uv;
out vec2 vTexCoord;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
	vTexCoord = in_uv;
}
)";

static std::string BuildFragmentSource(const postfx::Preset& preset, const postfx::PassDef& pass, bool forceOpaqueOutput)
{
	std::string s;
	s.reserve(pass.shaderBody.size() + 1024);

	s += R"(
in vec2 vTexCoord;
uniform sampler2D HC_Texture0;
uniform sampler2D HC_Texture1;
uniform sampler2D HC_Texture2;
uniform sampler2D HC_Texture3;
uniform sampler2D HC_Texture4;
uniform sampler2D HC_Texture5;
uniform sampler2D HC_Texture6;
uniform sampler2D HC_Texture7;
uniform vec2 SourceSize;
uniform vec2 NativeSize;
uniform vec2 OutputSize;
uniform vec2 WindowSize;
uniform int FrameCount;
uniform float Time;

#define Texture HC_Texture0
#define Source Texture
#define Original Texture
#define TextureSize SourceSize
#define InputSize SourceSize
#define OriginalSize SourceSize
#define TEX0 vTexCoord

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define mat4x4 mat4
#define CONSTANT const
#define frac(x) fract(x)
#define saturate(x) clamp((x), 0.0, 1.0)

#define GetOption(x) (x)

vec2 GetCoordinates() { return vTexCoord; }
vec2 GetFragCoord() { return gl_FragCoord.xy; }
vec2 GetResolution() { return SourceSize; }
vec2 GetInvResolution() { return vec2(1.0) / max(SourceSize, vec2(1.0)); }
vec2 GetViewportSize() { return OutputSize; }
vec2 GetNativeSize() { return NativeSize; }
vec2 GetNativePixelSize() { return OutputSize / max(NativeSize, vec2(1.0)); }
vec2 GetInvNativeSize() { return vec2(1.0) / max(NativeSize, vec2(1.0)); }
vec2 GetInvNativePixelSize() { return GetNativePixelSize() / max(WindowSize, vec2(1.0)); }
vec2 GetWindowToViewportRatio() { return WindowSize / max(OutputSize, vec2(1.0)); }
vec2 GetWindowSize() { return WindowSize; }
vec2 GetInvWindowSize() { return vec2(1.0) / max(WindowSize, vec2(1.0)); }
vec2 GetOriginalSize() { return SourceSize; }
vec2 GetPaddedOriginalSize() { return SourceSize * GetWindowToViewportRatio(); }
vec2 GetWindowResolution() { return WindowSize; }
vec2 GetInternalSize() { return SourceSize; }
vec2 GetInternalPixelSize() { return OutputSize / max(SourceSize, vec2(1.0)); }
vec2 GetInvInternalPixelSize() { return GetInternalPixelSize() / max(WindowSize, vec2(1.0)); }
float GetUpscaleMultiplier() { return SourceSize.x / max(NativeSize.x, 1.0); }
float GetTime() { return Time; }
int GetFrameCount() { return FrameCount; }

#define HC_Sample(s, uv) texture((s), (uv))
vec4 SampleLocation(vec2 uv) { return texture(Texture, uv); }
vec4 Sample() { return SampleLocation(vTexCoord); }
#define SampleOffset(offset) textureOffset(Texture, vTexCoord, offset)
vec4 HCSanitizeColor(vec4 c) { return vec4(
	(isnan(c.x) || isinf(c.x)) ? 0.0 : c.x,
	(isnan(c.y) || isinf(c.y)) ? 0.0 : c.y,
	(isnan(c.z) || isinf(c.z)) ? 0.0 : c.z,
	(isnan(c.w) || isinf(c.w)) ? 1.0 : c.w); }
void SetOutput(vec4 c) { FragColor = HCSanitizeColor(c); }

#define OptionEnabled(x) ((x) != 0)
)";
	s += std::string("#define HC_FORCE_OUTPUT_OPAQUE ") + (forceOpaqueOutput ? "1\n" : "0\n");
	s += R"(
void HC_SetOutputImpl(vec4 c) {
	vec4 hc = HCSanitizeColor(c);
#if HC_FORCE_OUTPUT_OPAQUE == 1
	FragColor = vec4(hc.rgb, 1.0);
#else
	FragColor = hc;
#endif
}
#undef SetOutput
#define SetOutput(c) HC_SetOutputImpl(c)
)";
	for (size_t i = 0; i < pass.samplers.size() && i < kMaxPostFxSamplers; i++)
	{
		const auto& sb = pass.samplers[i];
		if (sb.name.empty() || sb.name == "Texture")
			continue;
		s += "#define ";
		s += sb.name;
		s += " HC_Texture";
		s += std::to_string(i);
		s += "\n";
	}
	s += "\n";
	for (const auto& p : preset.params)
	{
		s += "uniform ";
		s += (p.integer ? "int " : "float ");
		s += p.id;
		s += ";\n";
	}
	s += "\n";
	std::string body = pass.shaderBody;
	size_t p = 0;
	while ((p = body.find("gl_FragColor", p)) != std::string::npos)
	{
		body.replace(p, 12, "FragColor");
		p += 9;
	}
	s += body;
	return s;
}

class PostFxChainGL
{
public:
	bool render(GLuint inputTex, int inputW, int inputH, GLuint outputFbo, int outX, int outY, int outW, int outH,
			bool rotate90, bool linearInterpolation)
	{
		postfx::Tick();
		const postfx::Preset* preset = postfx::GetActivePreset();
		if (!postfx::IsEnabled() || preset == nullptr || preset->passes.empty())
			return false;

		if (configVersion != postfx::GetConfigVersion() || viewportW != outW || viewportH != outH)
		{
			term();
			viewportW = outW;
			viewportH = outH;
			if (!init(*preset))
				return false;
		}

		// 1) Prepass: produce a "final-frame" base image at viewport resolution.
		if (sourceFbo == nullptr || sourceFbo->getWidth() != outW || sourceFbo->getHeight() != outH)
			sourceFbo = std::make_unique<GlFramebuffer>(outW, outH, false, true);

		sourceFbo->bind();
		glViewport(0, 0, outW, outH);
		glcache.Disable(GL_SCISSOR_TEST);
		glcache.Disable(GL_DEPTH_TEST);
		glcache.Disable(GL_STENCIL_TEST);
		glcache.Disable(GL_CULL_FACE);
		glcache.Disable(GL_BLEND);

		glcache.ClearColor(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.f);
		glClear(GL_COLOR_BUFFER_BIT);

		glActiveTexture(GL_TEXTURE0);
		glcache.BindTexture(GL_TEXTURE_2D, inputTex);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linearInterpolation ? GL_LINEAR : GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linearInterpolation ? GL_LINEAR : GL_NEAREST);
		const float* prepassVerts = nullptr;
		float sverts[20] = {
			-1.f, -1.f, 1.f, 0.f, 1.f,
			-1.f,  1.f, 1.f, 0.f, 0.f,
			 1.f, -1.f, 1.f, 1.f, 1.f,
			 1.f,  1.f, 1.f, 1.f, 0.f,
		};
		if (gl.ofbo.shiftX != 0 || gl.ofbo.shiftY != 0)
		{
			sverts[0] = sverts[5] = -1.f + gl.ofbo.shiftX * 2.f / (float)std::max(1, inputW);
			sverts[10] = sverts[15] = sverts[0] + 2.f;
			sverts[1] = sverts[11] = -1.f - gl.ofbo.shiftY * 2.f / (float)std::max(1, inputH);
			sverts[6] = sverts[16] = sverts[1] + 2.f;
			prepassVerts = sverts;
		}
		// Keep prepass alpha opaque even if source alpha is undefined in some OIT paths.
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		gl.quad->draw(inputTex, rotate90, true, prepassVerts);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// 2) PostFX passes.
		GLuint currentTex = sourceFbo->getTexture();
		int curW = outW;
		int curH = outH;

		const u32 frameCount = FrameCount;
		const float timeSeconds = (float)(frameCount) / 60.f;
		const float windowW = (float)(outW + outX * 2);
		const float windowH = (float)(outH + outY * 2);
		const float renderScale = std::max(1.f, config::RenderResolution.get() / 480.f);
		const float nativeW = std::max(1.f, (float)inputW / renderScale);
		const float nativeH = std::max(1.f, (float)inputH / renderScale);
		std::unordered_map<std::string, GLuint> namedTargets;

		for (size_t i = 0; i < passes.size(); i++)
		{
			auto& pass = passes[i];
			const bool isLast = (i + 1) == passes.size();

			int dstW = pass.outW;
			int dstH = pass.outH;
			GLuint dstFbo = outputFbo;

			if (!isLast)
			{
				if (pass.framebuffer == nullptr || pass.framebuffer->getWidth() != dstW || pass.framebuffer->getHeight() != dstH)
					pass.framebuffer = std::make_unique<GlFramebuffer>(dstW, dstH, false, true);
				dstFbo = pass.framebuffer->getFramebuffer();
			}

			glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
			if (isLast)
				glViewport(outX, outY, outW, outH);
			else
				glViewport(0, 0, dstW, dstH);

			glcache.UseProgram(pass.program);
			for (size_t unit = 0; unit < kMaxPostFxSamplers; unit++)
			{
				GLuint tex = currentTex;
				postfx::Filter filter = pass.filter;
				if (unit < pass.samplers.size())
				{
					const auto& sb = pass.samplers[unit];
					filter = sb.filter;
					if (sb.source != "$prev")
					{
						auto it = namedTargets.find(sb.source);
						if (it != namedTargets.end())
							tex = it->second;
					}
				}
				glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
				glcache.BindTexture(GL_TEXTURE_2D, tex);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter == postfx::Filter::Linear ? GL_LINEAR : GL_NEAREST);
				glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter == postfx::Filter::Linear ? GL_LINEAR : GL_NEAREST);
			}

			if (pass.sourceSizeUniform != -1)
				glUniform2f(pass.sourceSizeUniform, (float)curW, (float)curH);
			if (pass.nativeSizeUniform != -1)
				glUniform2f(pass.nativeSizeUniform, nativeW, nativeH);
			if (pass.outputSizeUniform != -1)
				glUniform2f(pass.outputSizeUniform, (float)dstW, (float)dstH);
			if (pass.windowSizeUniform != -1)
				glUniform2f(pass.windowSizeUniform, windowW, windowH);
			if (pass.frameCountUniform != -1)
				glUniform1i(pass.frameCountUniform, (int)frameCount);
			if (pass.timeUniform != -1)
				glUniform1f(pass.timeUniform, timeSeconds);

			for (auto& u : pass.paramUniforms)
			{
				float v = 0.f;
				if (postfx::GetParamValue(u.id, v))
				{
					if (u.integer)
						glUniform1i(u.loc, (int)std::lround(v));
					else
						glUniform1f(u.loc, v);
				}
			}

			if (isLast)
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
			vertexArray.bind(vertexBuffer.get());
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			GlVertexArray::unbind();
			if (isLast)
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

			if (!isLast)
			{
				currentTex = pass.framebuffer->getTexture();
				if (!pass.renderTargetName.empty())
					namedTargets[pass.renderTargetName] = currentTex;
				curW = dstW;
				curH = dstH;
			}
		}

		return true;
	}

	void term()
	{
		for (auto& p : passes)
		{
			if (p.program)
				glDeleteProgram(p.program);
			p.program = 0;
			p.framebuffer.reset();
		}
		passes.clear();
		sourceFbo.reset();
		vertexBuffer.reset();
		vertexArray.term();
		configVersion = 0;
		viewportW = viewportH = 0;
	}

private:
	struct CompiledPass
	{
		GLuint program = 0;
		GLint sourceSizeUniform = -1;
		GLint nativeSizeUniform = -1;
		GLint outputSizeUniform = -1;
		GLint windowSizeUniform = -1;
		GLint frameCountUniform = -1;
		GLint timeUniform = -1;
		struct ParamUniform {
			std::string id;
			GLint loc = -1;
			bool integer = false;
		};
		std::vector<ParamUniform> paramUniforms;
		postfx::Filter filter = postfx::Filter::Linear;
		std::string renderTargetName;
		std::vector<postfx::PassDef::SamplerBinding> samplers;
		int outW = 0;
		int outH = 0;
		std::unique_ptr<GlFramebuffer> framebuffer;
	};

	class VertexArray final : public GlVertexArray
	{
	protected:
		void defineVtxAttribs() override
		{
			glEnableVertexAttribArray(VERTEX_POS_ARRAY);
			glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)0);
			glEnableVertexAttribArray(VERTEX_UV_ARRAY);
			glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)(sizeof(float) * 3));
			glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
			glDisableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
			glDisableVertexAttribArray(VERTEX_COL_BASE1_ARRAY);
			glDisableVertexAttribArray(VERTEX_COL_OFFS1_ARRAY);
			glDisableVertexAttribArray(VERTEX_UV1_ARRAY);
			glDisableVertexAttribArray(VERTEX_NORM_ARRAY);
		}
	};

	bool init(const postfx::Preset& preset)
	{
		configVersion = postfx::GetConfigVersion();

		float vertices[] = {
			-1.f, -1.f, 1.f, 0.f, 0.f,
			 1.f, -1.f, 1.f, 1.f, 0.f,
			-1.f,  1.f, 1.f, 0.f, 1.f,
			 1.f,  1.f, 1.f, 1.f, 1.f,
		};
		vertexBuffer = std::make_unique<GlBuffer>(GL_ARRAY_BUFFER, GL_STATIC_DRAW);
		vertexBuffer->update(vertices, sizeof(vertices));

		passes.clear();
		passes.reserve(preset.passes.size());

		int curW = viewportW;
		int curH = viewportH;

		for (size_t i = 0; i < preset.passes.size(); i++)
		{
			const auto& passDef = preset.passes[i];
			CompiledPass pass;
			pass.filter = passDef.filter;
			pass.renderTargetName = passDef.renderTargetName;
			pass.samplers = passDef.samplers;

			int baseW = viewportW;
			int baseH = viewportH;
			switch (passDef.scaleType)
			{
			case postfx::ScaleType::Source:
				baseW = curW;
				baseH = curH;
				break;
			case postfx::ScaleType::Absolute:
				baseW = (int)std::round(passDef.scaleX);
				baseH = (int)std::round(passDef.scaleY);
				break;
			case postfx::ScaleType::Viewport:
			default:
				break;
			}
			if (passDef.scaleType != postfx::ScaleType::Absolute)
			{
				baseW = (int)std::round(baseW * passDef.scaleX);
				baseH = (int)std::round(baseH * passDef.scaleY);
			}
			pass.outW = std::max(1, baseW);
			pass.outH = std::max(1, baseH);
			if ((i + 1) == preset.passes.size())
			{
				// Final pass always renders into the current viewport region.
				pass.outW = viewportW;
				pass.outH = viewportH;
			}

			OpenGlSource vs;
			vs.addSource(VertexCompatShader).addSource(PostFxVertexSource);

			OpenGlSource fs;
			const bool isLast = ((i + 1) == preset.passes.size());
			const std::string frag = BuildFragmentSource(preset, passDef, isLast);
			fs.addSource(PixelCompatShader).addSource(frag.c_str());

			try
			{
				pass.program = gl_CompileAndLink(vs.generate().c_str(), fs.generate().c_str());
			}
			catch (const std::exception& e)
			{
				WARN_LOG(RENDERER, "PostFX pass compile failed: %s", e.what());
				postfx::SetRuntimeError(std::string("OpenGL pass compile failed: ") + e.what());
				return false;
			}
			if (pass.program == 0)
			{
				postfx::SetRuntimeError("OpenGL pass link failed.");
				return false;
			}

			glcache.UseProgram(pass.program);
			for (size_t unit = 0; unit < kMaxPostFxSamplers; unit++)
			{
				const std::string uname = "HC_Texture" + std::to_string(unit);
				GLint loc = glGetUniformLocation(pass.program, uname.c_str());
				if (loc != -1)
					glUniform1i(loc, (GLint)unit);
			}

			pass.sourceSizeUniform = glGetUniformLocation(pass.program, "SourceSize");
			pass.nativeSizeUniform = glGetUniformLocation(pass.program, "NativeSize");
			pass.outputSizeUniform = glGetUniformLocation(pass.program, "OutputSize");
			pass.windowSizeUniform = glGetUniformLocation(pass.program, "WindowSize");
			pass.frameCountUniform = glGetUniformLocation(pass.program, "FrameCount");
			pass.timeUniform = glGetUniformLocation(pass.program, "Time");

			for (const auto& p : preset.params)
			{
				GLint loc = glGetUniformLocation(pass.program, p.id.c_str());
				if (loc != -1)
					pass.paramUniforms.push_back({ p.id, loc, p.integer });
			}

			passes.push_back(std::move(pass));
			curW = passes.back().outW;
			curH = passes.back().outH;
		}

		postfx::ClearRuntimeError();
		return true;
	}

	std::unique_ptr<GlBuffer> vertexBuffer;
	VertexArray vertexArray;
	std::unique_ptr<GlFramebuffer> sourceFbo;
	std::vector<CompiledPass> passes;

	uint64_t configVersion = 0;
	int viewportW = 0;
	int viewportH = 0;
};

static PostFxChainGL s_chain;

} // namespace

namespace postfx_gl
{

bool render(GLuint inputTex, int inputW, int inputH, GLuint outputFbo,
		int outX, int outY, int outW, int outH, bool rotate90, bool linearInterpolation)
{
	return s_chain.render(inputTex, inputW, inputH, outputFbo, outX, outY, outW, outH, rotate90, linearInterpolation);
}

void term()
{
	s_chain.term();
}

} // namespace postfx_gl
#endif // !LIBRETRO
