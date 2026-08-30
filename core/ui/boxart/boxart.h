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
#include <array>
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
	std::string getLibraryCoverMediaPath(const GameMedia& media);
	std::string getCustomMediaPath(const GameMedia& media, config::LibraryCoverMediaMode mediaMode);
	void term();
	void refreshCache();
	void refreshLibraryPlaytimeDatabase();
	void refreshCustomBoxartIndex(bool force = false);
	void queueBoxart(const GameMedia& media);
	void startFetch();

private:
	enum class CustomIndexState
	{
		NotRequested,
		Building,
		Ready,
		Failed,
	};

	GameBoxart getPhysicalBoxart(const GameMedia& media);
	std::string getCustomBoxartPath(const GameMedia& media);
	std::string getCustomBoxartPathForMediaMode(const GameMedia& media, config::LibraryCoverMediaMode mediaMode,
			bool *indexReady = nullptr);
	GameBoxart getBoxartAndQueue(const GameMedia& media, bool startFetch);
	bool shouldFetchOnline() const;
	void loadDatabase();
	void loadLibraryPlaytimeDatabase();
	void applyLibraryPlaytimeUnlocked(GameBoxart& boxart) const;
	void recoverDatabases(const std::string& databaseDir, const std::string& artworkDir);
	void reviewDatabaseArtwork();
	void saveDatabase();
	void markDatabaseDirty();
	void buildCustomBoxartIndex();
	std::string getDatabaseDirectory() const;
	std::string getSaveDirectory() const {
		// File-system paths must end with a separator; Android SAF URIs must stay unchanged.
		if (!config::BoxartPath.get().empty()) {
			std::string path = config::BoxartPath.get();
			if (!path.empty() && path.find("content://") != 0 && path.back() != '/' && path.back() != '\\')
				path += '/';
			return path;
		}
		return get_writable_data_path("/boxart/");
	}
	void fetchBoxart();

	using CustomBoxartIndex = std::array<std::unordered_map<std::string, std::string>,
			static_cast<size_t>(config::LibraryCoverMediaMode::Count)>;
	std::unordered_map<std::string, GameBoxart> games;
	std::unordered_map<std::string, GameBoxart> physicalCache;
	std::unordered_map<std::string, u64> libraryPlaytimeByGameId;
	CustomBoxartIndex customBoxartByName;
	std::string customBoxartRoot;
	std::string requestedCustomBoxartRoot;
	std::mutex mutex;
	std::unique_ptr<Scraper> scraper;
	std::unique_ptr<Scraper> arcadeScraper;
	bool databaseLoaded = false;
	bool databaseDirty = false;
	// Incremented while mutex is held so a completed save never clears a newer update.
	u64 databaseGeneration = 0;
	bool customIndexLoaded = false;
	CustomIndexState customIndexState = CustomIndexState::NotRequested;
	u64 customIndexGeneration = 0;
	bool customIndexShuttingDown = false;

	std::vector<GameBoxart> toFetch;
	std::future<void> physicalFetching;
	std::future<void> onlineFetching;
	std::future<void> customIndexFetching;

	static constexpr char const *DB_NAME = "flycast-gamedb.json";
};
