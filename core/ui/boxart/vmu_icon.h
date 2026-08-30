/*
	Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.

	Hollycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Hollycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <cstddef>
#include <ctime>
#include <string>

struct GameMedia;

bool cacheVmuIconFromFlash(const std::string& gameId, const std::string& gameTitle, const void *data, size_t size);
void markLibraryGameBooted(const std::string& gameId, const std::string& gamePath);
time_t getLibraryGameLastBooted(const GameMedia& media, const std::string& gameId);
std::string getCachedVmuIconPath(const GameMedia& media, const std::string& gameId, bool animate = false, double animationClock = 0.0);
void clearVmuIconLookups();
