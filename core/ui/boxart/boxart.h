/*
	Copyright 2022 flyinghead
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
#pragma once
#include "scraper.h"
#include "stdclass.h"
#include "cfg/option.h"

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct GameMedia;

enum class BoxartSourceMode
{
	ScrapedOnly = 0,  // Only use scraped artwork from the box art database / online scraper
	PhysicalMediaOnly = 1, // Shows the Physical media image of the disc on the Rom
	CustomThenScraped = 2, // Use custom boxart, and then when no customs img is available, use scrapped media.
};

class Boxart
{
public:
	GameBoxart getBoxartAndLoad(const GameMedia& media);
	GameBoxart getBoxart(const GameMedia& media);
	void term();
	void refreshCache();
	void refreshCustomBoxartIndex(bool force = false);
	void queueBoxart(const GameMedia& media);
	void startFetch();

private:
	GameBoxart getPhysicalBoxart(const GameMedia& media);
	std::string getCustomBoxartPath(const GameMedia& media);
	GameBoxart getBoxartAndQueue(const GameMedia& media, bool startFetch);
	bool shouldFetchOnline() const;
	void loadDatabase();
	void recoverDatabases(const std::string& saveDir);
	void reviewDatabaseArtwork();
	void saveDatabase();
	std::string getSaveDirectory() const {
		// *must* end with a path separator
		if (!config::BoxartPath.get().empty()) {
			std::string path = config::BoxartPath.get();
			if (!path.empty() && path.back() != '/' && path.back() != '\\')
				path += '/';
			return path;
		}
		return get_writable_data_path("/boxart/");
	}
	void fetchBoxart();

	std::unordered_map<std::string, GameBoxart> games;
	std::unordered_map<std::string, GameBoxart> physicalCache;
	std::unordered_map<std::string, std::string> customBoxartByName;
	std::string customBoxartRoot;
	std::mutex mutex;
	std::unique_ptr<Scraper> scraper;
	std::unique_ptr<Scraper> arcadeScraper;
	bool databaseLoaded = false;
	bool databaseDirty = false;
	bool customIndexLoaded = false;

	std::vector<GameBoxart> toFetch;
	std::future<void> physicalFetching;
	std::future<void> onlineFetching;

	static constexpr char const *DB_NAME = "flycast-gamedb.json";
};
