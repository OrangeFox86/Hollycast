/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#include "postfx.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "oslib/resources.h"
#include "oslib/storage.h"
#include "stdclass.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <unordered_map>

namespace postfx
{

struct State
{
	bool enabled = false;
	std::string presetPath;

	Preset preset;
	bool presetLoaded = false;
	std::string error;

	std::unordered_map<std::string, float> values;
	uint64_t version = 1;
};

static State& state()
{
	static State s;
	return s;
}

static std::string sanitizeSection(std::string s)
{
	for (auto& c : s)
	{
		unsigned char uc = (unsigned char)c;
		if (!(std::isalnum(uc) || c == '_'))
			c = '_';
	}
	if (s.empty())
		s = "default";
	return "postfx." + s;
}

static const ParameterDef* findParamDef(const Preset& preset, const std::string& id)
{
	for (const auto& p : preset.params)
		if (p.id == id)
			return &p;
	return nullptr;
}

static float canonicalizeValue(const ParameterDef& p, float value)
{
	if (!std::isfinite(value))
		value = p.defaultValue;

	float v = std::clamp(value, p.minValue, p.maxValue);
	if (p.integer)
		return std::round(v);

	if (p.step > 0.f && std::isfinite(p.step))
	{
		const float n = std::round((v - p.minValue) / p.step);
		v = p.minValue + n * p.step;
		v = std::clamp(v, p.minValue, p.maxValue);
	}
	return v;
}

static void reloadPreset(State& s)
{
	s.presetLoaded = false;
	s.preset = Preset{};
	s.values.clear();
	s.error.clear();

	if (!s.enabled)
	{
		s.version++;
		return;
	}
	if (s.presetPath.empty())
	{
		s.error = "PostFX preset is not set.";
		s.version++;
		return;
	}

	std::string err;
	if (!LoadPreset(s.presetPath, s.preset, err))
	{
		s.error = err;
		s.version++;
		return;
	}

	const std::string section = sanitizeSection(s.presetPath);
	for (const auto& p : s.preset.params)
	{
		const float raw = config::loadFloat(section, p.id, p.defaultValue);
		const float v = canonicalizeValue(p, raw);
		s.values.emplace(p.id, v);
		if (v != raw)
			config::saveFloat(section, p.id, v);
	}

	s.presetLoaded = true;
	s.version++;
}

void Tick()
{
	State& s = state();
	bool enabled = (bool)config::PostFxEnabled;
	std::string preset = (std::string)config::PostFxPreset;

	if (enabled != s.enabled || preset != s.presetPath)
	{
		s.enabled = enabled;
		s.presetPath = std::move(preset);
		reloadPreset(s);
	}
}

bool IsEnabled()
{
	return state().enabled;
}

uint64_t GetConfigVersion()
{
	return state().version;
}

const Preset* GetActivePreset()
{
	State& s = state();
	return s.presetLoaded ? &s.preset : nullptr;
}

const std::string& GetLastError()
{
	return state().error;
}

std::vector<std::string> ListBuiltInPresets()
{
	std::vector<std::string> out;
	for (const auto& f : resource::listDirectory("shaders/postfx/presets"))
		out.push_back("shaders/postfx/presets/" + f);
	std::sort(out.begin(), out.end());
	return out;
}

static bool hasExt(const std::string& path, const char* ext)
{
	const std::string e = get_file_extension(path);
	return e == ext;
}

static void collectExternalPresets(const std::string& root, std::vector<std::string>& out)
{
	if (root.empty())
		return;
	try
	{
		if (!hostfs::storage().exists(root))
			return;
		std::vector<hostfs::FileInfo> dirs;
		dirs.emplace_back(root, root, true);
		while (!dirs.empty())
		{
			const std::string dir = dirs.back().path;
			dirs.pop_back();
			for (const auto& e : hostfs::storage().listContent(dir))
			{
				if (e.isDirectory)
				{
					dirs.push_back(e);
					continue;
				}
				if (hasExt(e.name, "ini") || hasExt(e.name, "glsl") || hasExt(e.name, "fx"))
					out.push_back(e.path);
			}
		}
	}
	catch (const FlycastException&)
	{
	}
}

std::vector<std::string> ListAvailablePresets()
{
	static std::vector<std::string> cached;
	static bool scanned = false;
	if (!scanned)
	{
		scanned = true;
		cached = ListBuiltInPresets();
		collectExternalPresets(get_writable_data_path("shaders"), cached);
		std::unordered_set<std::string> seen;
		std::vector<std::string> dedup;
		dedup.reserve(cached.size());
		for (auto& p : cached)
			if (seen.insert(p).second)
				dedup.push_back(std::move(p));
		std::sort(dedup.begin(), dedup.end());
		cached = std::move(dedup);
	}
	return cached;
}

bool GetParamValue(const std::string& id, float& outValue)
{
	State& s = state();
	auto it = s.values.find(id);
	if (it == s.values.end())
		return false;
	outValue = it->second;
	return true;
}

void SetParamValue(const std::string& id, float value)
{
	State& s = state();
	auto it = s.values.find(id);
	if (it == s.values.end())
		return;
	if (const auto* p = findParamDef(s.preset, id))
		value = canonicalizeValue(*p, value);
	if (it->second == value)
		return;
	it->second = value;
	config::saveFloat(sanitizeSection(s.presetPath), id, value);
	s.version++;
}

void SetRuntimeError(const std::string& error)
{
	State& s = state();
	if (!s.enabled || !s.presetLoaded)
		return;
	if (s.error == error)
		return;
	s.error = error;
}

void ClearRuntimeError()
{
	State& s = state();
	if (!s.enabled || !s.presetLoaded)
		return;
	if (s.error.empty())
		return;
	s.error.clear();
}

} // namespace postfx
