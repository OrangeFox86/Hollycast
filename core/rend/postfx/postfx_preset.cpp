/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#include "postfx_preset.h"

#include "cfg/ini.h"
#include "oslib/resources.h"
#include "stdclass.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace postfx
{

static inline std::string trim(std::string s)
{
	auto notSpace = [](unsigned char c) { return !std::isspace(c); };
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
	s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	return s;
}

static bool iequals(const std::string& a, const std::string& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
			return false;
	return true;
}

static const char* findSection(const config::IniFile& ini, const char* a, const char* b)
{
	if (ini.hasSection(a))
		return a;
	if (b && ini.hasSection(b))
		return b;
	return a; // default
}

static bool loadText(const std::string& path, std::string& out, std::string& error)
{
	size_t size = 0;
	if (auto data = resource::load(path, size))
	{
		out.assign((const char*)data.get(), (const char*)data.get() + size);
		return true;
	}

	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		error = "Unable to load: " + path;
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (len <= 0)
	{
		std::fclose(f);
		error = "Empty file: " + path;
		return false;
	}
	out.resize((size_t)len);
	const size_t read = std::fread(out.data(), 1, (size_t)len, f);
	std::fclose(f);
	if (read != (size_t)len)
	{
		error = "Short read: " + path;
		return false;
	}
	return true;
}

static bool parseFloat(const std::string& s, float& out)
{
	char* end = nullptr;
	out = std::strtof(s.c_str(), &end);
	return end && end != s.c_str();
}

static std::string joinPath(const std::string& base, const std::string& leaf)
{
	if (base.empty())
		return leaf;
	char sep = '/';
	if (base.find('\\') != std::string::npos && base.find('/') == std::string::npos)
		sep = '\\';
	if (base.back() == '/' || base.back() == '\\')
		return base + leaf;
	return base + sep + leaf;
}

static std::string resolveShaderPath(const std::string& presetPath, const std::string& shaderPath)
{
	if (shaderPath.empty())
		return shaderPath;
	if (isAbsolutePath(shaderPath))
		return shaderPath;
	if (shaderPath.rfind("shaders/", 0) == 0)
		return shaderPath;
	if (file_exists(shaderPath))
		return shaderPath;
	const size_t slash = get_last_slash_pos(presetPath);
	if (slash == std::string::npos)
		return shaderPath;
	const std::string base = presetPath.substr(0, slash);
	const std::string candidate = joinPath(base, shaderPath);
	if (file_exists(candidate))
		return candidate;
	return shaderPath;
}

std::vector<ParameterDef> ParsePragmaParameters(const std::string& shaderText)
{
	std::vector<ParameterDef> out;
	size_t pos = 0;
	while (pos < shaderText.size())
	{
		size_t lineEnd = shaderText.find('\n', pos);
		if (lineEnd == std::string::npos)
			lineEnd = shaderText.size();
		std::string line = trim(shaderText.substr(pos, lineEnd - pos));
		pos = lineEnd + 1;

		if (line.rfind("#pragma", 0) != 0)
			continue;
		if (line.find("parameter") == std::string::npos)
			continue;

		// Format: #pragma parameter <id> "<label>" <def> <min> <max> <step>
		size_t p = line.find("parameter");
		if (p == std::string::npos)
			continue;
		p += std::strlen("parameter");
		while (p < line.size() && std::isspace((unsigned char)line[p]))
			p++;

		size_t idStart = p;
		while (p < line.size() && !std::isspace((unsigned char)line[p]))
			p++;
		if (p <= idStart)
			continue;
		std::string id = line.substr(idStart, p - idStart);

		while (p < line.size() && std::isspace((unsigned char)line[p]))
			p++;

		std::string label = id;
		if (p < line.size() && line[p] == '"')
		{
			p++;
			size_t q = line.find('"', p);
			if (q == std::string::npos)
				continue;
			label = line.substr(p, q - p);
			p = q + 1;
		}

		auto nextToken = [&](std::string& tok) -> bool {
			while (p < line.size() && std::isspace((unsigned char)line[p]))
				p++;
			if (p >= line.size())
				return false;
			size_t s = p;
			while (p < line.size() && !std::isspace((unsigned char)line[p]))
				p++;
			tok = line.substr(s, p - s);
			return true;
		};

		std::string tok;
		ParameterDef def;
		def.id = id;
		def.label = label;
		if (!nextToken(tok) || !parseFloat(tok, def.defaultValue))
			continue;
		if (!nextToken(tok) || !parseFloat(tok, def.minValue))
			continue;
		if (!nextToken(tok) || !parseFloat(tok, def.maxValue))
			continue;
		if (nextToken(tok))
			(void)parseFloat(tok, def.step);

		out.emplace_back(std::move(def));
	}
	return out;
}

static std::vector<ParameterDef> ParseDolphinFxConfiguration(const std::string& shaderText)
{
	const size_t a = shaderText.find("[configuration]");
	if (a == std::string::npos)
		return {};
	const size_t b = shaderText.find("[/configuration]", a);
	if (b == std::string::npos)
		return {};

	std::vector<ParameterDef> out;
	ParameterDef cur;
	bool inOpt = false;
	bool isInt = false;

	auto flush = [&]() {
		if (!inOpt || cur.id.empty())
			return;
		cur.integer = isInt;
		if (cur.label.empty())
			cur.label = cur.id;
		if (cur.step <= 0.f)
			cur.step = cur.integer ? 1.f : 0.01f;
		out.emplace_back(std::move(cur));
		cur = ParameterDef{};
		inOpt = false;
		isInt = false;
	};

	size_t pos = a;
	while (pos < b)
	{
		size_t lineEnd = shaderText.find('\n', pos);
		if (lineEnd == std::string::npos || lineEnd > b)
			lineEnd = b;
		std::string line = trim(shaderText.substr(pos, lineEnd - pos));
		pos = lineEnd + 1;

		if (line.empty())
			continue;
		if (line == "[OptionRangeFloat]")
		{
			flush();
			inOpt = true;
			isInt = false;
			continue;
		}
		if (line == "[OptionRangeInteger]")
		{
			flush();
			inOpt = true;
			isInt = true;
			continue;
		}
		if (line.size() > 2 && line.front() == '[' && line.back() == ']')
			continue;
		if (!inOpt)
			continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = trim(line.substr(0, eq));
		const std::string val = trim(line.substr(eq + 1));

		if (iequals(key, "GUIName"))
			cur.label = val;
		else if (iequals(key, "OptionName"))
			cur.id = val;
		else if (iequals(key, "MinValue"))
			(void)parseFloat(val, cur.minValue);
		else if (iequals(key, "MaxValue"))
			(void)parseFloat(val, cur.maxValue);
		else if (iequals(key, "StepAmount"))
			(void)parseFloat(val, cur.step);
		else if (iequals(key, "DefaultValue"))
			(void)parseFloat(val, cur.defaultValue);
	}
	flush();

	return out;
}

static std::vector<ParameterDef> InferGetOptionParameters(const std::string& shaderText)
{
	std::unordered_set<std::string> ids;
	size_t pos = 0;
	while ((pos = shaderText.find("GetOption(", pos)) != std::string::npos)
	{
		pos += 10;
		while (pos < shaderText.size() && std::isspace((unsigned char)shaderText[pos]))
			pos++;
		size_t start = pos;
		if (start >= shaderText.size())
			break;
		const unsigned char c0 = (unsigned char)shaderText[start];
		if (!(std::isalpha(c0) || shaderText[start] == '_'))
			continue;
		pos++;
		while (pos < shaderText.size())
		{
			const unsigned char c = (unsigned char)shaderText[pos];
			if (!(std::isalnum(c) || shaderText[pos] == '_'))
				break;
			pos++;
		}
		ids.insert(shaderText.substr(start, pos - start));
	}

	std::vector<ParameterDef> out;
	out.reserve(ids.size());
	for (const auto& id : ids)
	{
		ParameterDef p;
		p.id = id;
		p.label = id;
		out.emplace_back(std::move(p));
	}
	return out;
}

static std::vector<ParameterDef> ParseShaderParameters(const std::string& shaderText)
{
	auto cfg = ParseDolphinFxConfiguration(shaderText);
	if (!cfg.empty())
		return cfg;
	auto pragma = ParsePragmaParameters(shaderText);
	if (!pragma.empty())
		return pragma;
	if (shaderText.find("GetOption(") != std::string::npos)
		return InferGetOptionParameters(shaderText);
	return {};
}

static std::vector<ParameterDef> ParseFxUniformParameters(const std::string& shaderText)
{
	std::vector<ParameterDef> out;
	std::unordered_set<std::string> seen;

	const std::regex re(
		R"(uniform\s+(float|int|bool)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:<([\s\S]*?)>)?\s*=\s*([-\+0-9.eE]+|true|false)\s*;)");

	for (auto it = std::sregex_iterator(shaderText.begin(), shaderText.end(), re);
			it != std::sregex_iterator(); ++it)
	{
		ParameterDef p;
		const std::string ty = (*it)[1].str();
		p.id = (*it)[2].str();
		if (!seen.insert(p.id).second)
			continue;
		p.label = p.id;
		p.integer = (ty == "int" || ty == "bool");
		p.step = p.integer ? 1.f : 0.01f;
		p.minValue = p.integer ? 0.f : 0.f;
		p.maxValue = p.integer ? 1.f : 1.f;
		const std::string def = (*it)[4].str();
		if (def == "true")
			p.defaultValue = 1.f;
		else if (def == "false")
			p.defaultValue = 0.f;
		else
			(void)parseFloat(def, p.defaultValue);

		const std::string meta = (*it)[3].str();
		if (!meta.empty())
		{
			const std::regex minRe(R"(ui_min\s*=\s*([-\+0-9.eE]+))");
			const std::regex maxRe(R"(ui_max\s*=\s*([-\+0-9.eE]+))");
			const std::regex stepRe(R"(ui_step\s*=\s*([-\+0-9.eE]+))");
			std::smatch m;
			if (std::regex_search(meta, m, minRe))
				(void)parseFloat(m[1].str(), p.minValue);
			if (std::regex_search(meta, m, maxRe))
				(void)parseFloat(m[1].str(), p.maxValue);
			if (std::regex_search(meta, m, stepRe))
				(void)parseFloat(m[1].str(), p.step);
		}
		out.push_back(std::move(p));
	}

	return out;
}

static std::string stripConfigurationBlock(const std::string& shaderText)
{
	std::string out = shaderText;
	const size_t a = out.find("[configuration]");
	if (a == std::string::npos)
		return out;
	const size_t b = out.find("[/configuration]", a);
	if (b == std::string::npos)
		return out;
	out.erase(a, b + std::strlen("[/configuration]") - a);
	return out;
}

static std::string resolveIncludePathForFx(const std::string& sourcePath, const std::string& includePath)
{
	if (isAbsolutePath(includePath))
		return includePath;
	const size_t slash = get_last_slash_pos(sourcePath);
	if (slash == std::string::npos)
		return includePath;
	return joinPath(sourcePath.substr(0, slash), includePath);
}

static bool preprocessFxIncludes(const std::string& sourcePath, const std::string& shaderText, int depth,
		std::unordered_set<std::string>& visited, std::string& outText, std::string& error)
{
	if (depth > 8)
	{
		error = "Exceeded include depth while parsing .fx: " + sourcePath;
		return false;
	}

	std::ostringstream out;
	size_t pos = 0;
	while (pos < shaderText.size())
	{
		size_t eol = shaderText.find('\n', pos);
		if (eol == std::string::npos)
			eol = shaderText.size();
		std::string line = shaderText.substr(pos, eol - pos);
		pos = eol + 1;

		std::string t = trim(line);
		if (t.rfind("#include", 0) == 0)
		{
			size_t q0 = t.find('"');
			size_t q1 = (q0 == std::string::npos) ? std::string::npos : t.find('"', q0 + 1);
			if (q0 == std::string::npos || q1 == std::string::npos || q1 <= q0 + 1)
			{
				error = "Malformed #include in .fx file: " + sourcePath;
				return false;
			}
			const std::string inc = t.substr(q0 + 1, q1 - q0 - 1);
			const std::string incPath = resolveIncludePathForFx(sourcePath, inc);
			if (visited.find(incPath) != visited.end())
				continue;
			visited.insert(incPath);

			std::string incText;
			if (!loadText(incPath, incText, error))
			{
				// Allow missing UI helper includes; they only define metadata macros in common packs.
				if (inc.find("ReShadeUI.fxh") != std::string::npos)
					continue;
				return false;
			}
			// ReShade framework headers define runtime interfaces/types we replace with
			// HollyCast builtins in the generated prelude.
			if (inc.find("ReShade.fxh") != std::string::npos || inc.find("ReShadeUI.fxh") != std::string::npos)
				continue;

			std::string expanded;
			if (!preprocessFxIncludes(incPath, incText, depth + 1, visited, expanded, error))
				return false;
			out << expanded << "\n";
			continue;
		}
		out << line << "\n";
	}

	outText = out.str();
	return true;
}

static std::string findFxEntryPoint(const std::string& shaderText)
{
	const std::regex techRe(R"(PixelShader\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)");
	std::smatch m;
	if (std::regex_search(shaderText, m, techRe))
		return m[1].str();

	const std::regex fnRe(R"((?:float4|vec4)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*(?:float2|vec2)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*:[^,\)]*)?\s*\))");
	if (std::regex_search(shaderText, m, fnRe))
		return m[1].str();
	return {};
}

struct FxPassSpec
{
	std::string vertexShader;
	std::string pixelShader;
	std::string renderTarget;
};

static std::vector<FxPassSpec> ParseFxTechniquePasses(const std::string& shaderText)
{
	std::vector<FxPassSpec> out;
	size_t pos = 0;
	while (true)
	{
		size_t tech = shaderText.find("technique", pos);
		if (tech == std::string::npos)
			break;
		size_t open = shaderText.find('{', tech);
		if (open == std::string::npos)
			break;
		int depth = 1;
		size_t i = open + 1;
		for (; i < shaderText.size() && depth > 0; i++)
		{
			if (shaderText[i] == '{') depth++;
			else if (shaderText[i] == '}') depth--;
		}
		if (depth != 0)
			break;
		const std::string block = shaderText.substr(open + 1, (i - 1) - (open + 1));

		size_t ppos = 0;
		while (true)
		{
			size_t pass = block.find("pass", ppos);
			if (pass == std::string::npos)
				break;
			size_t pob = block.find('{', pass);
			if (pob == std::string::npos)
				break;
			int pdepth = 1;
			size_t pj = pob + 1;
			for (; pj < block.size() && pdepth > 0; pj++)
			{
				if (block[pj] == '{') pdepth++;
				else if (block[pj] == '}') pdepth--;
			}
			if (pdepth != 0)
				break;
			const std::string passBlock = block.substr(pob + 1, (pj - 1) - (pob + 1));
			std::smatch m;
			FxPassSpec spec;
			if (std::regex_search(passBlock, m, std::regex(R"(VertexShader\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)")))
				spec.vertexShader = m[1].str();
			if (std::regex_search(passBlock, m, std::regex(R"(PixelShader\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)")))
				spec.pixelShader = m[1].str();
			if (std::regex_search(passBlock, m, std::regex(R"(RenderTarget\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)")))
				spec.renderTarget = m[1].str();
			if (!spec.pixelShader.empty())
				out.push_back(std::move(spec));
			ppos = pj;
		}

		if (!out.empty())
			return out;
		pos = i;
	}
	return out;
}

struct FxSamplerDef
{
	std::string textureRef;
	Filter filter = Filter::Linear;
};

static std::unordered_map<std::string, FxSamplerDef> ParseFxSamplers(const std::string& shaderText)
{
	std::unordered_map<std::string, FxSamplerDef> out;
	const std::regex re(R"(\bsampler(?:2D)?\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([\s\S]*?)\}\s*;)");
	for (auto it = std::sregex_iterator(shaderText.begin(), shaderText.end(), re);
			it != std::sregex_iterator(); ++it)
	{
		const std::string name = (*it)[1].str();
		const std::string body = (*it)[2].str();
		FxSamplerDef def;
		std::smatch m;
		if (std::regex_search(body, m, std::regex(R"(\bTexture\s*=\s*([A-Za-z_][A-Za-z0-9_:]*)\s*;)")))
			def.textureRef = m[1].str();
		if (std::regex_search(body, m, std::regex(R"(\b(?:MinFilter|MagFilter)\s*=\s*POINT\b)")))
			def.filter = Filter::Nearest;
		out[name] = std::move(def);
	}
	return out;
}

static std::vector<std::string> ParseUsedFxSamplers(const std::string& shaderText)
{
	std::vector<std::string> out;
	std::unordered_set<std::string> seen;
	const std::regex re(R"(\bHC_Sample\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)");
	for (auto it = std::sregex_iterator(shaderText.begin(), shaderText.end(), re);
			it != std::sregex_iterator(); ++it)
	{
		const std::string name = (*it)[1].str();
		if (seen.insert(name).second)
			out.push_back(name);
	}
	return out;
}

static std::string rewriteTex2DCalls(const std::string& shaderText)
{
	std::string out;
	out.reserve(shaderText.size());
	size_t i = 0;
	while (i < shaderText.size())
	{
		const size_t p = shaderText.find("tex2D(", i);
		if (p == std::string::npos)
		{
			out.append(shaderText, i, std::string::npos);
			break;
		}
		out.append(shaderText, i, p - i);
		size_t j = p + 6; // after tex2D(
		int depth = 1;
		size_t comma = std::string::npos;
		while (j < shaderText.size() && depth > 0)
		{
			const char c = shaderText[j];
			if (c == '(')
				depth++;
			else if (c == ')')
				depth--;
			else if (c == ',' && depth == 1 && comma == std::string::npos)
				comma = j;
			j++;
		}
		if (depth != 0 || comma == std::string::npos)
		{
			out += "tex2D(";
			i = p + 6;
			continue;
		}
		size_t arg1Start = p + 6;
		while (arg1Start < comma && std::isspace((unsigned char)shaderText[arg1Start]))
			arg1Start++;
		size_t arg1End = comma;
		while (arg1End > arg1Start && std::isspace((unsigned char)shaderText[arg1End - 1]))
			arg1End--;

		size_t arg2Start = comma + 1;
		while (arg2Start < j - 1 && std::isspace((unsigned char)shaderText[arg2Start]))
			arg2Start++;
		size_t arg2End = j - 1;
		while (arg2End > arg2Start && std::isspace((unsigned char)shaderText[arg2End - 1]))
			arg2End--;
		out += "HC_Sample(";
		out.append(shaderText, arg1Start, arg1End - arg1Start);
		out += ", ";
		out.append(shaderText, arg2Start, arg2End - arg2Start);
		out += ")";
		i = j;
	}
	return out;
}

static std::string rewriteTex2DlodCalls(const std::string& shaderText)
{
	std::string out;
	out.reserve(shaderText.size());
	size_t i = 0;
	while (i < shaderText.size())
	{
		const size_t p = shaderText.find("tex2Dlod(", i);
		if (p == std::string::npos)
		{
			out.append(shaderText, i, std::string::npos);
			break;
		}
		out.append(shaderText, i, p - i);
		size_t j = p + 9; // after tex2Dlod(
		int depth = 1;
		size_t comma = std::string::npos;
		while (j < shaderText.size() && depth > 0)
		{
			const char c = shaderText[j];
			if (c == '(')
				depth++;
			else if (c == ')')
				depth--;
			else if (c == ',' && depth == 1 && comma == std::string::npos)
				comma = j;
			j++;
		}
		if (depth != 0 || comma == std::string::npos)
		{
			out += "tex2Dlod(";
			i = p + 9;
			continue;
		}
		size_t arg1Start = p + 9;
		while (arg1Start < comma && std::isspace((unsigned char)shaderText[arg1Start]))
			arg1Start++;
		size_t arg1End = comma;
		while (arg1End > arg1Start && std::isspace((unsigned char)shaderText[arg1End - 1]))
			arg1End--;

		size_t arg2Start = comma + 1;
		while (arg2Start < j - 1 && std::isspace((unsigned char)shaderText[arg2Start]))
			arg2Start++;
		size_t arg2End = j - 1;
		while (arg2End > arg2Start && std::isspace((unsigned char)shaderText[arg2End - 1]))
			arg2End--;
		std::string arg2 = shaderText.substr(arg2Start, arg2End - arg2Start);
		const size_t c2 = arg2.find(',');
		if (c2 != std::string::npos)
			arg2 = trim(arg2.substr(0, c2));
		out += "HC_Sample(";
		out.append(shaderText, arg1Start, arg1End - arg1Start);
		out += ", (" + arg2 + ").xy)";
		i = j;
	}
	return out;
}

static std::string stripFxTechniques(const std::string& shaderText)
{
	std::string out;
	out.reserve(shaderText.size());
	size_t pos = 0;
	while (pos < shaderText.size())
	{
		const size_t tech = shaderText.find("technique", pos);
		if (tech == std::string::npos)
		{
			out.append(shaderText, pos, std::string::npos);
			break;
		}
		out.append(shaderText, pos, tech - pos);
		size_t open = shaderText.find('{', tech);
		if (open == std::string::npos)
		{
			// Keep trailing text if malformed.
			out.append(shaderText, tech, std::string::npos);
			break;
		}
		int depth = 1;
		size_t i = open + 1;
		for (; i < shaderText.size() && depth > 0; i++)
		{
			if (shaderText[i] == '{')
				depth++;
			else if (shaderText[i] == '}')
				depth--;
		}
		pos = i;
	}
	return out;
}

static bool findFxFunctionSignature(const std::string& body, const std::string& entry,
		std::string& returnType, std::string& argList)
{
	const std::regex fn(
		"\\b(float4|vec4|float3|vec3|void)\\s+" + entry + R"(\s*\(([^)]*)\))");
	std::smatch m;
	if (!std::regex_search(body, m, fn))
		return false;
	returnType = m[1].str();
	argList = m[2].str();
	return true;
}

static std::string buildFxMainWrapper(const std::string& body, const std::string& entry)
{
	if (entry.empty())
		return {};

	std::string returnType;
	std::string argList;
	if (!findFxFunctionSignature(body, entry, returnType, argList))
		return {};

	std::vector<std::string> args;
	{
		std::string cur;
		int d = 0;
		for (char c : argList)
		{
			if (c == '(') d++;
			if (c == ')') d--;
			if (c == ',' && d == 0)
			{
				args.push_back(trim(cur));
				cur.clear();
			}
			else
			{
				cur.push_back(c);
			}
		}
		if (!trim(cur).empty())
			args.push_back(trim(cur));
	}

	bool hasOut4 = false;
	bool hasOut3 = false;
	std::vector<std::string> callArgs;
	callArgs.reserve(args.size());
	for (const auto& a : args)
	{
		const std::string t = trim(a);
		const bool isOut = (t.find("out ") != std::string::npos) || (t.find("inout ") != std::string::npos);
		if (isOut && (t.find("float4") != std::string::npos || t.find("vec4") != std::string::npos))
		{
			hasOut4 = true;
			callArgs.push_back("hc_out4");
		}
		else if (isOut && (t.find("float3") != std::string::npos || t.find("vec3") != std::string::npos))
		{
			hasOut3 = true;
			callArgs.push_back("hc_out3");
		}
		else if (t.find("float2") != std::string::npos || t.find("vec2") != std::string::npos)
		{
			callArgs.push_back("hc_uv");
		}
		else if (t.find("float4") != std::string::npos || t.find("vec4") != std::string::npos)
		{
			callArgs.push_back("vec4(0.0)");
		}
		else if (t.find("float3") != std::string::npos || t.find("vec3") != std::string::npos)
		{
			callArgs.push_back("vec3(0.0)");
		}
		else if (t.find("int2") != std::string::npos)
		{
			callArgs.push_back("ivec2(0)");
		}
		else if (t.find("int") != std::string::npos)
		{
			callArgs.push_back("0");
		}
		else
		{
			callArgs.push_back("0.0");
		}
	}

	std::string call;
	for (size_t i = 0; i < callArgs.size(); i++)
	{
		if (i) call += ", ";
		call += callArgs[i];
	}

	std::string w = "\nvoid main()\n{\n\tvec2 hc_uv = GetCoordinates();\n";
	if (hasOut4)
		w += "\tvec4 hc_out4 = vec4(0.0);\n";
	if (hasOut3)
		w += "\tvec3 hc_out3 = vec3(0.0);\n";

	if (returnType == "float4" || returnType == "vec4")
		w += "\tSetOutput(" + entry + "(" + call + "));\n";
	else if (returnType == "float3" || returnType == "vec3")
		w += "\tSetOutput(vec4(" + entry + "(" + call + "), 1.0));\n";
	else
	{
		w += "\t" + entry + "(" + call + ");\n";
		if (hasOut4)
			w += "\tSetOutput(hc_out4);\n";
		else if (hasOut3)
			w += "\tSetOutput(vec4(hc_out3, 1.0));\n";
		else
			w += "\tSetOutput(Sample());\n";
	}
	w += "}\n";
	return w;
}

static std::string ConvertFxToPostFxGlsl(const std::string& shaderText, const std::string& forcedEntry, std::string& error)
{
	std::string body = stripConfigurationBlock(shaderText);
	const std::string entry = forcedEntry.empty() ? findFxEntryPoint(body) : forcedEntry;
	if (entry.empty())
	{
		error = "Unsupported .fx file: unable to find pixel shader entry point.";
		return {};
	}

	body = stripFxTechniques(body);
	body = rewriteTex2DCalls(body);
	body = rewriteTex2DlodCalls(body);
	// Remove HLSL semantics only in function declaration lines.
	{
		std::string cleaned;
		cleaned.reserve(body.size());
		size_t pos = 0;
		while (pos < body.size())
		{
			size_t eol = body.find('\n', pos);
			if (eol == std::string::npos)
				eol = body.size();
			std::string line = body.substr(pos, eol - pos);
			pos = eol + 1;

			const bool declLike = (line.find('(') != std::string::npos
					&& line.find(')') != std::string::npos
					&& line.find('{') != std::string::npos);
			if (declLike)
			{
				size_t i = 0;
				while ((i = line.find(':', i)) != std::string::npos)
				{
					if (i > 0 && line[i - 1] == '?')
					{
						i++;
						continue;
					}
					size_t j = i + 1;
					while (j < line.size() && std::isspace((unsigned char)line[j]))
						j++;
					size_t k = j;
					while (k < line.size() && (std::isalnum((unsigned char)line[k]) || line[k] == '_'))
						k++;
					if (k > j)
						line.erase(i, k - i);
					else
						i++;
				}
			}
			cleaned.append(line);
			cleaned.push_back('\n');
		}
		body = std::move(cleaned);
	}

	// Remove common FX declarations we do not support in v1.
	body = std::regex_replace(body, std::regex(R"(\btexture(?:1D|2D|3D|Cube)?\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:<[^>]*>)?\s*(?:\{[^}]*\})?\s*;)"), "");
	body = std::regex_replace(body, std::regex(R"(\bsampler(?:2D)?\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:\{[^}]*\})?\s*;)"), "");
	body = std::regex_replace(body, std::regex(R"(\bSV_Target\b)"), "");
	body = std::regex_replace(body, std::regex(R"(\bSV_Position\b)"), "");
	body = std::regex_replace(body, std::regex(R"(\bTEXCOORD\d*\b)"), "");
	body = std::regex_replace(body, std::regex(R"(\bTexCoord\b)"), "");
	body = std::regex_replace(body, std::regex(R"(\bSV_TARGET\d*\b)"), "");
	body = std::regex_replace(body, std::regex(R"(ReShade::BackBufferTex)"), "Texture");
	body = std::regex_replace(body, std::regex(R"(ReShade::BackBuffer)"), "Texture");
	body = std::regex_replace(body, std::regex(R"(\blerp\s*\()"), "mix(");
	body = std::regex_replace(body, std::regex(R"(\bfrac\s*\()"), "fract(");

	// We expose pragma/config-derived params ourselves, strip FX uniform declarations.
	body = std::regex_replace(body, std::regex(R"(\buniform\s+(?:float|int|bool)\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:<[^>]*>)?\s*=\s*[^;]+;)"), "");
	body = std::regex_replace(body, std::regex(R"(\buniform\s+(?:float|int|bool|float2|float3|float4|int2|int3|int4)\s+[A-Za-z_][A-Za-z0-9_]*\s*<[^>]*>\s*;)"), "");

	std::string prelude = R"(
#define BUFFER_WIDTH (SourceSize.x)
#define BUFFER_HEIGHT (SourceSize.y)
#define BUFFER_RCP_WIDTH (1.0 / max(SourceSize.x, 1.0))
#define BUFFER_RCP_HEIGHT (1.0 / max(SourceSize.y, 1.0))
#define BUFFER_ASPECT_RATIO (SourceSize.x / max(SourceSize.y, 1.0))
#define BUFFER_SCREEN_SIZE (SourceSize)
#define BufferWidth (SourceSize.x)
#define BufferHeight (SourceSize.y)
#define ViewportSize (OutputSize)
#define ViewportWidth (OutputSize.x)
#define ViewportHeight (OutputSize.y)
#define BufferToViewportRatio GetWindowToViewportRatio()
#define NormalizedNativePixelSize GetInvNativePixelSize()
#define UpscaleMultiplier GetUpscaleMultiplier()
#define timer Time
#define FCount FrameCount
)";
	body = prelude + "\n" + body;

	if (!std::regex_search(body, std::regex(R"(\bvoid\s+main\s*\(\s*\))")))
	{
		const std::string wrapper = buildFxMainWrapper(body, entry);
		if (wrapper.empty())
		{
			error = "Unsupported .fx file: cannot wrap selected PixelShader entry '" + entry + "'.";
			return {};
		}
		body += wrapper;
	}

	return body;
}

static bool isBuiltinLine(const std::string& trimmedLine)
{
	// Strip only a small set of well-known builtins to avoid breaking arbitrary shaders.
	static const char* const patterns[] = {
		"uniform sampler2D Texture",
		"uniform sampler2D Source",
		"uniform sampler2D Original",
		"uniform vec2 SourceSize",
		"uniform vec2 OutputSize",
		"uniform vec2 TextureSize",
		"uniform vec2 InputSize",
		"uniform vec2 OriginalSize",
		"uniform int FrameCount",
		"uniform float FrameCount",
		"uniform float Time",
		"in vec2 vTexCoord",
		"varying vec2 vTexCoord",
		"out vec4 FragColor",
	};
	for (auto* pat : patterns)
		if (trimmedLine.rfind(pat, 0) == 0)
			return true;
	return false;
}

std::string StripBuiltinsAndPragmas(const std::string& shaderText)
{
	static const std::vector<ParameterDef> noParams;
	return StripBuiltinsAndPragmas(shaderText, noParams);
}

std::string StripBuiltinsAndPragmas(const std::string& shaderText, const std::vector<ParameterDef>& params)
{
	std::string out;
	out.reserve(shaderText.size());

	size_t pos = 0;
	while (pos < shaderText.size())
	{
		size_t lineEnd = shaderText.find('\n', pos);
		if (lineEnd == std::string::npos)
			lineEnd = shaderText.size();
		std::string line = shaderText.substr(pos, lineEnd - pos);
		pos = lineEnd + 1;

		std::string t = trim(line);
		if (t.rfind("#version", 0) == 0)
			continue;
		if (t.rfind("#pragma", 0) == 0 && t.find("parameter") != std::string::npos)
			continue;
		if (isBuiltinLine(t))
			continue;
		if (t.rfind("uniform float ", 0) == 0)
		{
			// Common community GLSL packs declare uniforms for pragma parameters.
			// We source parameter values ourselves, so strip these to avoid backend-specific layout issues.
			for (const auto& p : params)
			{
				const std::string decl = std::string("uniform float ") + p.id;
				if (t.rfind(decl, 0) == 0)
					goto nextLine;
			}
		}
		if (t.rfind("uniform int ", 0) == 0)
		{
			for (const auto& p : params)
			{
				if (!p.integer)
					continue;
				const std::string decl = std::string("uniform int ") + p.id;
				if (t.rfind(decl, 0) == 0)
					goto nextLine;
			}
		}

		out.append(line);
		out.push_back('\n');
nextLine:
		(void)0;
	}

	return out;
}

static ScaleType parseScaleType(const std::string& s)
{
	if (iequals(s, "source"))
		return ScaleType::Source;
	if (iequals(s, "absolute"))
		return ScaleType::Absolute;
	return ScaleType::Viewport;
}

static Filter parseFilter(const std::string& s)
{
	if (iequals(s, "nearest") || iequals(s, "point"))
		return Filter::Nearest;
	return Filter::Linear;
}

bool LoadPreset(const std::string& presetPath, Preset& out, std::string& error)
{
	try
	{
	const std::string ext = get_file_extension(presetPath);
	if (ext == "glsl" || ext == "fx")
	{
		std::string shaderText;
		if (!loadText(presetPath, shaderText, error))
			return false;
		if (ext == "fx")
		{
			std::string expanded;
			std::unordered_set<std::string> visited;
			visited.insert(presetPath);
			if (!preprocessFxIncludes(presetPath, shaderText, 0, visited, expanded, error))
				return false;
			shaderText = std::move(expanded);
		}
		out = Preset{};
		out.id = presetPath;
		const size_t slash = get_last_slash_pos(presetPath);
		const std::string leaf = slash == std::string::npos ? presetPath : presetPath.substr(slash + 1);
		out.name = get_file_basename(leaf);
		out.params = (ext == "fx") ? ParseFxUniformParameters(shaderText) : ParseShaderParameters(shaderText);
		if (ext == "fx")
		{
			if (std::regex_search(shaderText, std::regex(R"(\btexture\w*\s+[A-Za-z_][A-Za-z0-9_]*\s*<[^>]*\bsource\s*=\s*")")))
			{
				error = "Unsupported .fx file: external texture sources are not supported in HollyCast v1.";
				return false;
			}

			const auto fxSamplers = ParseFxSamplers(shaderText);
			const auto fxPasses = ParseFxTechniquePasses(shaderText);
			if (!fxPasses.empty())
			{
				out.passes.clear();
				out.passes.reserve(fxPasses.size());
				for (const auto& fxp : fxPasses)
				{
					if (!fxp.vertexShader.empty() && fxp.vertexShader != "PostProcessVS")
					{
						error = "Unsupported .fx pass: custom vertex shader '" + fxp.vertexShader + "' is not supported in HollyCast v1.";
						return false;
					}
					PassDef pass;
					pass.shaderPath = presetPath;
					pass.scaleType = ScaleType::Viewport;
					pass.scaleX = 1.f;
					pass.scaleY = 1.f;
					pass.filter = Filter::Linear;
					pass.renderTargetName = fxp.renderTarget;

					std::string fxErr;
					const std::string converted = ConvertFxToPostFxGlsl(shaderText, fxp.pixelShader, fxErr);
					if (converted.empty())
					{
						error = fxErr.empty() ? ("Unsupported .fx file: " + presetPath) : fxErr;
						return false;
					}
					const auto usedSamplers = ParseUsedFxSamplers(converted);
					pass.samplers.push_back({ "Texture", "$prev", pass.filter });
					for (const auto& sname : usedSamplers)
					{
						if (sname == "Texture" || sname == "Source" || sname == "Original")
							continue;
						auto it = fxSamplers.find(sname);
						PassDef::SamplerBinding sb;
						sb.name = sname;
						sb.source = "$prev";
						sb.filter = Filter::Linear;
						if (it != fxSamplers.end())
						{
							sb.filter = it->second.filter;
							if (!it->second.textureRef.empty()
									&& it->second.textureRef != "ReShade::BackBufferTex"
									&& it->second.textureRef != "ReShade::BackBuffer")
								sb.source = it->second.textureRef;
						}
						pass.samplers.push_back(std::move(sb));
					}
					if (pass.samplers.size() > 8)
					{
						error = "Unsupported .fx pass: requires more than 8 samplers.";
						return false;
					}
					pass.shaderBody = StripBuiltinsAndPragmas(converted, out.params);
					out.passes.push_back(std::move(pass));
				}
				return true;
			}

			PassDef pass;
			pass.shaderPath = presetPath;
			pass.scaleType = ScaleType::Viewport;
			pass.scaleX = 1.f;
			pass.scaleY = 1.f;
			pass.filter = Filter::Linear;
			std::string fxErr;
			const std::string converted = ConvertFxToPostFxGlsl(shaderText, {}, fxErr);
			if (converted.empty())
			{
				error = fxErr.empty() ? ("Unsupported .fx file: " + presetPath) : fxErr;
				return false;
			}
			const auto usedSamplers = ParseUsedFxSamplers(converted);
			pass.samplers.push_back({ "Texture", "$prev", pass.filter });
			for (const auto& sname : usedSamplers)
			{
				if (sname == "Texture" || sname == "Source" || sname == "Original")
					continue;
				auto it = fxSamplers.find(sname);
				PassDef::SamplerBinding sb;
				sb.name = sname;
				sb.source = "$prev";
				sb.filter = Filter::Linear;
				if (it != fxSamplers.end())
				{
					sb.filter = it->second.filter;
					if (!it->second.textureRef.empty()
							&& it->second.textureRef != "ReShade::BackBufferTex"
							&& it->second.textureRef != "ReShade::BackBuffer")
						sb.source = it->second.textureRef;
				}
				pass.samplers.push_back(std::move(sb));
			}
			if (pass.samplers.size() > 8)
			{
				error = "Unsupported .fx pass: requires more than 8 samplers.";
				return false;
			}
			pass.shaderBody = StripBuiltinsAndPragmas(converted, out.params);
			out.passes.push_back(std::move(pass));
			return true;
		}

		PassDef pass;
		pass.shaderPath = presetPath;
		pass.scaleType = ScaleType::Viewport;
		pass.scaleX = 1.f;
		pass.scaleY = 1.f;
		pass.filter = Filter::Linear;
		pass.samplers.push_back({ "Texture", "$prev", pass.filter });
		pass.shaderBody = StripBuiltinsAndPragmas(shaderText, out.params);
		out.passes.push_back(std::move(pass));
		return true;
	}

	std::string iniText;
	if (!loadText(presetPath, iniText, error))
		return false;

	config::IniFile ini;
	ini.load(iniText);

	out = Preset{};
	out.id = presetPath;

	const char* presetSection = findSection(ini, "Preset", "preset");
	out.name = ini.getString(presetSection, "name", presetPath);

	const int passCount = std::max(1, ini.getInt(presetSection, "passes", 1));
	out.passes.resize((size_t)passCount);

	std::unordered_map<std::string, size_t> paramIndex;

	for (int i = 0; i < passCount; i++)
	{
		char secA[32];
		char secB[32];
		std::snprintf(secA, sizeof(secA), "Pass%d", i);
		std::snprintf(secB, sizeof(secB), "pass%d", i);
		const char* passSection = findSection(ini, secA, secB);

		PassDef& pass = out.passes[(size_t)i];
		pass.shaderPath = ini.getString(passSection, "shader", {});
		if (pass.shaderPath.empty())
		{
			error = "Preset is missing shader path for " + std::string(passSection);
			return false;
		}
		pass.shaderPath = resolveShaderPath(presetPath, pass.shaderPath);

		pass.scaleType = parseScaleType(ini.getString(passSection, "scale_type", "viewport"));
		pass.scaleX = ini.getFloat(passSection, "scale_x", 1.f);
		pass.scaleY = ini.getFloat(passSection, "scale_y", 1.f);
		pass.filter = parseFilter(ini.getString(passSection, "filter", "linear"));
		pass.srgb = ini.getBool(passSection, "srgb", false);
		pass.samplers.push_back({ "Texture", "$prev", pass.filter });

		std::string shaderText;
		if (!loadText(pass.shaderPath, shaderText, error))
			return false;

		const auto shaderParams = ParseShaderParameters(shaderText);
		for (const auto& p : shaderParams)
		{
			auto it = paramIndex.find(p.id);
			if (it == paramIndex.end())
			{
				paramIndex.emplace(p.id, out.params.size());
				out.params.push_back(p);
			}
		}

		pass.shaderBody = StripBuiltinsAndPragmas(shaderText, shaderParams);
	}

	const char* paramsSection = findSection(ini, "Parameters", "parameters");
	if (ini.hasSection(paramsSection))
	{
		for (const auto& name : ini.getEntryNames(paramsSection))
		{
			auto it = paramIndex.find(name);
			if (it == paramIndex.end())
				continue;
			out.params[it->second].defaultValue = ini.getFloat(paramsSection, name, out.params[it->second].defaultValue);
		}
	}

	return true;
	}
	catch (const std::regex_error& e)
	{
		error = std::string("Shader preset regex parse failed: ") + e.what();
		return false;
	}
}

} // namespace postfx
