/*
	Copyright 2022 flyinghead

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
#include "boxart.h"
#include "gamesdb.h"
#include "../game_scanner.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "cfg/option.h"
#include "arcade_scraper.h"
#include <chrono>

namespace {

bool isSupportedBoxartExtension(const std::string& ext)
{
	return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp";
}

std::string normalizeBoxartKey(const std::string& value)
{
	std::string out;
	out.reserve(value.size());
	bool prevSpace = true;
	for (unsigned char c : value)
	{
		if (std::isalnum(c))
		{
			out.push_back(static_cast<char>(std::tolower(c)));
			prevSpace = false;
		}
		else if (!prevSpace)
		{
			out.push_back(' ');
			prevSpace = true;
		}
	}
	return trim_ws(out);
}

std::string makeBoxartKey(const std::string& value)
{
	return normalizeBoxartKey(get_file_basename(value));
}

} // namespace

GameBoxart Boxart::getBoxart(const GameMedia& media)
{
	loadDatabase();
	const int sourceMode = config::BoxartSourceMode.get();
	if (sourceMode == static_cast<int>(BoxartSourceMode::PhysicalOnly))
		return getPhysicalBoxart(media);

	GameBoxart boxart;
	{
		std::lock_guard<std::mutex> guard(mutex);
		auto it = games.find(media.fileName);
		if (it != games.end())
			boxart = it->second;
	}

	if (sourceMode == static_cast<int>(BoxartSourceMode::CustomThenScraped))
	{
		const std::string customPath = getCustomBoxartPath(media);
		if (!customPath.empty())
			boxart.boxartPath = customPath;
	}
	else if (sourceMode == static_cast<int>(BoxartSourceMode::ScrapedOnly))
	{
		if (boxart.boxartUrl.empty())
			boxart.boxartPath.clear();
	}

	return boxart;
}

GameBoxart Boxart::getBoxartAndLoad(const GameMedia& media)
{
	loadDatabase();
	const int sourceMode = config::BoxartSourceMode.get();
	if (sourceMode == static_cast<int>(BoxartSourceMode::PhysicalOnly))
		return getPhysicalBoxart(media);

	GameBoxart boxart;
	{
		std::lock_guard<std::mutex> guard(mutex);
		auto it = games.find(media.fileName);
		if (it != games.end())
		{
			boxart = it->second;
			const bool wantOnline = shouldFetchOnline();
			const bool needsOffline = !boxart.parsed;
			const bool needsOnline = wantOnline && !boxart.scraped;
			if (!boxart.busy && (needsOffline || needsOnline))
			{
				boxart.busy = it->second.busy = true;
				boxart.gamePath = media.path;
				boxart.arcade = media.arcade;
				toFetch.push_back(boxart);
			}
		}
		else
		{
			boxart.fileName = media.fileName;
			boxart.gamePath = media.path;
			boxart.name = media.name;
			boxart.searchName = media.gameName;	// for arcade games
			boxart.busy = true;
			boxart.arcade = media.arcade;
			games[boxart.fileName] = boxart;
			toFetch.push_back(boxart);
		}
	}
	fetchBoxart();

	if (sourceMode == static_cast<int>(BoxartSourceMode::CustomThenScraped))
	{
		const std::string customPath = getCustomBoxartPath(media);
		if (!customPath.empty())
			boxart.boxartPath = customPath;
	}
	else if (sourceMode == static_cast<int>(BoxartSourceMode::ScrapedOnly))
	{
		if (boxart.boxartUrl.empty())
			boxart.boxartPath.clear();
	}

	return boxart;
}

bool Boxart::shouldFetchOnline() const
{
	return config::FetchBoxart && config::BoxartSourceMode.get() != static_cast<int>(BoxartSourceMode::PhysicalOnly);
}

GameBoxart Boxart::getPhysicalBoxart(const GameMedia& media)
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		auto it = physicalCache.find(media.fileName);
		if (it != physicalCache.end())
			return it->second;
	}

	GameBoxart boxart;
	boxart.fileName = media.fileName;
	boxart.gamePath = media.path;
	boxart.name = media.name;
	boxart.searchName = media.gameName;
	boxart.arcade = media.arcade;

	OfflineScraper physicalScraper;
	physicalScraper.initialize(getSaveDirectory());
	physicalScraper.scrape(boxart);

	{
		std::lock_guard<std::mutex> guard(mutex);
		physicalCache[boxart.fileName] = boxart;
	}

	return boxart;
}

std::string Boxart::getCustomBoxartPath(const GameMedia& media)
{
	refreshCustomBoxartIndex(false);

	const std::string fileKey = makeBoxartKey(media.fileName);
	const std::string nameKey = makeBoxartKey(media.name);
	const std::string gameNameKey = makeBoxartKey(media.gameName);

	std::lock_guard<std::mutex> guard(mutex);
	if (!fileKey.empty())
	{
		auto it = customBoxartByName.find(fileKey);
		if (it != customBoxartByName.end())
			return it->second;
	}
	if (!nameKey.empty())
	{
		auto it = customBoxartByName.find(nameKey);
		if (it != customBoxartByName.end())
			return it->second;
	}
	if (!gameNameKey.empty())
	{
		auto it = customBoxartByName.find(gameNameKey);
		if (it != customBoxartByName.end())
			return it->second;
	}
	return {};
}

void Boxart::fetchBoxart()
{
	if (fetching.valid() && fetching.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			fetching.get();
		} catch (const std::exception& e) {
			ERROR_LOG(COMMON, "Boxart scraper thread exception: %s", e.what());
		} catch (...) {
			ERROR_LOG(COMMON, "Boxart scraper thread unknown exception");
		}
	}
	if (fetching.valid())
		return;
	if (toFetch.empty())
		return;
	fetching = std::async(std::launch::async, [this]() {
		ThreadName _("BoxArt-scraper");
		const bool wantOnline = shouldFetchOnline();
		if (offlineScraper == nullptr)
		{
			offlineScraper = std::unique_ptr<Scraper>(new OfflineScraper());
			offlineScraper->initialize(getSaveDirectory());
		}
		if (wantOnline)
		{
			if (scraper == nullptr)
			{
				scraper = std::unique_ptr<Scraper>(new TheGamesDb());
				if (!scraper->initialize(getSaveDirectory()))
				{
					ERROR_LOG(COMMON, "thegamesdb scraper initialization failed");
					scraper.reset();
					return;
				}
			}
			if (arcadeScraper == nullptr) {
				arcadeScraper = std::make_unique<ArcadeScraper>();
				arcadeScraper->initialize(getSaveDirectory());
			}
		}
		std::vector<GameBoxart> boxart;
		{
			std::lock_guard<std::mutex> guard(mutex);
			size_t size = std::min(toFetch.size(), (size_t)10);
			boxart = std::vector<GameBoxart>(toFetch.begin(), toFetch.begin() + size);
			toFetch.erase(toFetch.begin(), toFetch.begin() + size);
		}
		DEBUG_LOG(COMMON, "Scraping %d games", (int)boxart.size());
		offlineScraper->scrape(boxart);
		{
			std::lock_guard<std::mutex> guard(mutex);
			for (GameBoxart& b : boxart)
				if (b.scraped || b.parsed)
				{
					if (!wantOnline || b.scraped)
						b.busy = false;
					games[b.fileName] = b;
					databaseDirty = true;
				}
		}
		if (wantOnline)
		{
			try {
				arcadeScraper->scrape(boxart);
				scraper->scrape(boxart);
				{
					std::lock_guard<std::mutex> guard(mutex);
					for (GameBoxart& b : boxart)
					{
						b.busy = false;
						games[b.fileName] = b;
					}
				}
				databaseDirty = true;
			} catch (const std::runtime_error& e) {
				if (*e.what() != '\0')
					INFO_LOG(COMMON, "thegamesdb error: %s", e.what());
				{
					// put back failed items into toFetch array
					std::lock_guard<std::mutex> guard(mutex);
					for (GameBoxart& b : boxart)
						if (b.scraped)
						{
							b.busy = false;
							games[b.fileName] = b;
							databaseDirty = true;
						}
						else
						{
							toFetch.push_back(b);
						}
				}
			}
		}
		saveDatabase();
	});
}

void Boxart::saveDatabase()
{
	if (!databaseDirty)
		return;
	std::string basePath = getSaveDirectory();
	std::string db_name = basePath + DB_NAME;
	DEBUG_LOG(COMMON, "Saving boxart database to %s", db_name.c_str());

	json array;
	{
		std::lock_guard<std::mutex> guard(mutex);
		for (const auto& game : games)
			if (game.second.scraped || game.second.parsed)
				array.push_back(game.second.to_json(basePath));
	}
	std::string serialized = array.dump(4, ' ', false, json::error_handler_t::replace);

	FILE *file = nowide::fopen(db_name.c_str(), "wt");
	if (file == nullptr) {
		WARN_LOG(COMMON, "Can't save boxart database to %s: error %d", db_name.c_str(), errno);
		return;
	}
	fwrite(serialized.c_str(), 1, serialized.size(), file);
	fclose(file);
	databaseDirty = false;
}

void Boxart::loadDatabase()
{
	if (databaseLoaded)
		return;
	databaseLoaded = true;
	databaseDirty = false;
	std::string save_dir = getSaveDirectory();
	if (!file_exists(save_dir))
		make_directory(save_dir);
	std::string db_name = save_dir + DB_NAME;
	FILE *f = nowide::fopen(db_name.c_str(), "rt");
	if (f == nullptr)
	{
		refreshCustomBoxartIndex(false);
		return;
	}

	DEBUG_LOG(COMMON, "Loading boxart database from %s", db_name.c_str());
	std::string all_data;
	char buf[4096];
	while (true)
	{
		int s = fread(buf, 1, sizeof(buf), f);
		if (s <= 0)
			break;
		all_data.append(buf, s);
	}
	fclose(f);
	try {
		std::lock_guard<std::mutex> guard(mutex);

		json v = json::parse(all_data);
		for (const auto& o : v)
		{
			GameBoxart game(o, save_dir);
			games[game.fileName] = game;
		}
	} catch (const json::exception& e) {
		WARN_LOG(COMMON, "Corrupted database file: %s", e.what());
	}
	refreshCustomBoxartIndex(false);
}

void Boxart::term()
{
	if (fetching.valid())
		fetching.get();
}

void Boxart::refreshCustomBoxartIndex(bool force)
{
	const std::string root = getSaveDirectory();
	if (!force && customIndexLoaded && root == customBoxartRoot)
		return;

	std::unordered_map<std::string, std::string> newIndex;
	if (!root.empty() && file_exists(root))
	{
		try {
			hostfs::DirectoryTree tree(root);
			for (auto it = tree.begin(); it != tree.end(); ++it)
			{
				const hostfs::FileInfo& entry = *it;
				if (entry.isDirectory)
					continue;
				const std::string ext = get_file_extension(entry.name);
				if (!isSupportedBoxartExtension(ext))
					continue;
				const std::string key = makeBoxartKey(entry.name);
				if (key.empty())
					continue;
				newIndex[key] = entry.path;
			}
		} catch (const std::exception& e) {
			WARN_LOG(COMMON, "Custom boxart scan failed: %s", e.what());
		}
	}

	{
		std::lock_guard<std::mutex> guard(mutex);
		if (root != customBoxartRoot)
			physicalCache.clear();
		customBoxartRoot = root;
		customBoxartByName.swap(newIndex);
		customIndexLoaded = true;
	}
}
