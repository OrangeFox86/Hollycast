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
#include "boxart.h"
#include "gamesdb.h"
#include "../game_scanner.h"
#include "oslib/oslib.h"
#include "oslib/directory.h"
#include "oslib/storage.h"
#include "cfg/option.h"
#include "arcade_scraper.h"
#include <nowide/convert.hpp>
#include <nowide/stackstring.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

static constexpr size_t kOnlineBoxartBatchSize = 10;
static constexpr size_t kPhysicalBoxartBatchSize = 40;
static constexpr size_t kPhysicalBoxartWorkerCount = 4;
static constexpr size_t kLibraryPlaytimeDatabaseMaxSize = 4 * 1024 * 1024;
static constexpr size_t kLibraryPlaytimeDatabaseMaxEntries = 100000;
static constexpr const char *LIBRARY_PLAYTIME_DB_NAME = "library-playtime.json";

bool isContentUri(const std::string& path)
{
	return path.rfind("content://", 0) == 0;
}

std::string appendFilesystemPath(const std::string& directory, const std::string& name)
{
	if (directory.empty() || directory.back() == '/' || directory.back() == '\\')
		return directory + name;
#ifdef _WIN32
	return directory + "\\" + name;
#else
	return directory + "/" + name;
#endif
}

bool ensureFilesystemDirectory(const std::string& path)
{
	// Android SAF paths can persist in shared settings. Reject content URIs in
	// every build so native filesystem APIs never treat one as a directory.
	return !path.empty() && !isContentUri(path) && (file_exists(path) || make_directory(path));
}

bool isWritableFilesystemDirectory(const std::string& path)
{
	if (!ensureFilesystemDirectory(path))
		return false;
	try {
		const hostfs::FileInfo info = hostfs::storage().getFileInfo(path);
		return info.isDirectory && info.isWritable;
	} catch (const hostfs::StorageException&) {
		return false;
	}
}

std::string stableDatabaseKey(const std::string& path)
{
	u64 hash = 14695981039346656037ULL;
	for (unsigned char c : path)
	{
		hash ^= c;
		hash *= 1099511628211ULL;
	}
	char key[17];
	std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
	return key;
}

bool readStorageFile(const std::string& path, std::string& output, size_t maximumSize = std::numeric_limits<size_t>::max())
{
	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(path, "rb"));
	if (file == nullptr)
		return false;
	const s64 size = file->size();
	if (size < 0 || static_cast<u64>(size) > maximumSize)
		return false;
	output.resize(static_cast<size_t>(size));
	return output.empty() || file->read(output.data(), 1, output.size()) == output.size();
}

std::string normalizeLibraryGameId(std::string value)
{
	value = trim_ws(value);
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return value;
}

std::string getLibraryPlaytimeDatabasePath(const std::string& root)
{
	try {
		return isContentUri(root)
			? hostfs::storage().getSubPath(root, LIBRARY_PLAYTIME_DB_NAME)
			: appendFilesystemPath(root, LIBRARY_PLAYTIME_DB_NAME);
	} catch (const hostfs::StorageException&) {
		return {};
	}
}

bool replaceFileAtomically(const std::string& temporaryPath, const std::string& path)
{
#ifdef _WIN32
	nowide::wstackstring temporary, destination;
	return temporary.convert(temporaryPath.c_str())
		&& destination.convert(path.c_str())
		&& MoveFileExW(temporary.get(), destination.get(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return flycast::rename(temporaryPath.c_str(), path.c_str()) == 0;
#endif
}

bool writeFileAtomically(const std::string& path, const std::string& contents)
{
	const std::string temporaryPath = path + ".tmp";
	FILE *file = nowide::fopen(temporaryPath.c_str(), "wb");
	if (file == nullptr)
		return false;
	bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
	if (std::fflush(file) != 0)
		written = false;
	if (std::fclose(file) != 0)
		written = false;
	if (!written)
	{
		nowide::remove(temporaryPath.c_str());
		return false;
	}
	if (replaceFileAtomically(temporaryPath, path))
		return true;
	nowide::remove(temporaryPath.c_str());
	return false;
}

bool isSupportedBoxartExtension(const std::string& ext)
{
	std::string lowerExt = ext;
	std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return lowerExt == "png" || lowerExt == "jpg" || lowerExt == "jpeg" || lowerExt == "bmp";
}

bool isSupportedManualExtension(const std::string& ext)
{
	std::string lowerExt = ext;
	std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return isSupportedBoxartExtension(lowerExt) || lowerExt == "pdf" || lowerExt == "cbz" || lowerExt == "cbr";
}

std::string normalizeBoxartKey(const std::string& value)
{
	std::string out;
	out.reserve(value.size());
	bool prevSpace = true;
	for (unsigned char c : value)
	{
		if (c < 0x80 && std::isalnum(c))
		{
			out.push_back(static_cast<char>(std::tolower(c)));
			prevSpace = false;
		}
		else if (c >= 0x80)
		{
			out.push_back(static_cast<char>(c));
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

std::string makeBoxartKey(const std::string& filepath)
{
	return normalizeBoxartKey(get_file_basename(filepath));
}

GameBoxart makeMediaBoxart(const GameMedia& media)
{
	GameBoxart boxart;
	boxart.fileName = media.fileName;
	boxart.gamePath = media.path;
	boxart.name = media.name;
	boxart.searchName = media.gameName;
	boxart.arcade = media.arcade;
	return boxart;
}

bool isMissingBoxartFile(const GameBoxart& boxart)
{
	return !boxart.boxartPath.empty() && !file_exists(boxart.boxartPath);
}

bool isMissingScrapedBoxartFile(const GameBoxart& boxart)
{
	return boxart.scraped && !boxart.boxartUrl.empty() && (boxart.boxartPath.empty() || isMissingBoxartFile(boxart));
}

size_t findLastPathSeparator(const std::string& path)
{
#ifdef _WIN32
	// Windows accepts both separators in paths. POSIX treats backslash as a
	// valid filename character, so only split on '/' outside Windows.
	return path.find_last_of("/\\");
#else
	return path.find_last_of('/');
#endif
}

std::string parentPath(const std::string& path)
{
	const size_t pos = findLastPathSeparator(path);
	if (pos == std::string::npos)
		return {};
	return path.substr(0, pos + 1);
}

std::string fileNameFromPath(const std::string& path)
{
	const size_t pos = findLastPathSeparator(path);
	if (pos == std::string::npos)
		return path;
	return path.substr(pos + 1);
}

// Boxart database recovery may need to match copied artwork such as
// "Game (1).png" back to an older database entry that still points at
// "Game.png", or the other way around.
std::string removeCopySuffix(std::string name)
{
	const std::string ext = get_file_extension(name);
	if (ext.empty())
		return name;

	std::string base = get_file_basename(name);
	const size_t closeParen = base.rfind(')');
	const size_t openParen = base.rfind(" (");
	if (closeParen == base.length() - 1 && openParen != std::string::npos && openParen < closeParen)
	{
		bool allDigits = true;
		for (size_t i = openParen + 2; i < closeParen; i++)
			if (!std::isdigit(static_cast<unsigned char>(base[i])))
			{
				allDigits = false;
				break;
			}
		if (allDigits)
			base = base.substr(0, openParen);
	}
	return base + "." + ext;
}

std::string normalizePath(std::string path)
{
#ifdef _WIN32
	// Backslash is only a path separator on Windows. POSIX platforms can use it
	// in filenames, so do not rewrite it there.
	std::replace(path.begin(), path.end(), '\\', '/');
#endif
	std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return path;
}

bool isPathInDirectory(const std::string& path, const std::string& directory)
{
	const std::string normalizedPath = normalizePath(path);
	const std::string normalizedDirectory = normalizePath(directory);
	return normalizedPath.substr(0, normalizedDirectory.length()) == normalizedDirectory;
}

int boxartMergeScore(const GameBoxart& boxart)
{
	int score = 0;
	const bool hasBoxartFile = !boxart.boxartPath.empty() && file_exists(boxart.boxartPath);
	if (boxart.scraped && !boxart.boxartUrl.empty())
		score += hasBoxartFile ? 100 : 70;
	if (hasBoxartFile)
		score += 20;
	if (boxart.parsed)
		score += 10;
	if (boxart.scraped)
		score += 5;
	if (!boxart.overview.empty())
		score += 2;
	if (!boxart.name.empty())
		score += 1;
	return score;
}

void scrapePhysicalBoxart(std::vector<GameBoxart>& boxart, const std::string& saveDirectory)
{
	std::atomic<size_t> nextIndex{ 0 };
	const size_t workerCount = std::min(kPhysicalBoxartWorkerCount, boxart.size());
	std::vector<std::future<void>> workers;
	workers.reserve(workerCount);
	for (size_t i = 0; i < workerCount; i++)
	{
		workers.push_back(std::async(std::launch::async, [&boxart, &nextIndex, saveDirectory]() {
			OfflineScraper scraper;
			scraper.initialize(saveDirectory);
			while (true)
			{
				const size_t index = nextIndex.fetch_add(1);
				if (index >= boxart.size())
					break;
				scraper.scrape(boxart[index]);
			}
		}));
	}
	for (std::future<void>& worker : workers)
		worker.get();
}

#ifdef __ANDROID__
int hexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

std::string decodeUriComponent(const std::string& value)
{
	// Android SAF encodes document IDs in URIs: "primary%3ARoms%2Fcovers" becomes "primary:Roms/covers".
	std::string decoded;
	decoded.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i)
	{
		if (value[i] == '%' && i + 2 < value.size())
		{
			const int hi = hexValue(value[i + 1]);
			const int lo = hexValue(value[i + 2]);
			if (hi >= 0 && lo >= 0)
			{
				decoded.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		decoded.push_back(value[i]);
	}
	return decoded;
}

std::string getAndroidDocumentId(const std::string& uri, const char* marker)
{
	// The shared media scanner uses this only for Android Storage Access Framework content:// URIs.
	const std::string decoded = decodeUriComponent(uri);
	const size_t markerPos = decoded.find(marker);
	if (markerPos == std::string::npos)
		return {};
	const size_t idStart = markerPos + std::strlen(marker);
	const size_t idEnd = std::strcmp(marker, "/tree/") == 0 ? decoded.find("/document/", idStart) : std::string::npos;
	return decoded.substr(idStart, idEnd == std::string::npos ? std::string::npos : idEnd - idStart);
}
#endif

std::string getParentPath(const std::string& root, const std::string& path)
{
	// SAF content URIs only exist on Android. Other platforms use filesystem paths.
	#ifdef __ANDROID__
	if (root.find("content://") == 0 || path.find("content://") == 0)
	{
		const std::string rootId = getAndroidDocumentId(root, "/tree/");
		const std::string pathId = getAndroidDocumentId(path, "/document/");
		if (rootId.empty() || pathId.size() <= rootId.size() || pathId.compare(0, rootId.size(), rootId) != 0)
			return {};
		std::string relative = pathId.substr(rootId.size());
		while (!relative.empty() && relative.front() == '/')
			relative.erase(relative.begin());
		const size_t slash = relative.find('/');
		return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
	}
	#endif

	if (path.size() <= root.size() || path.compare(0, root.size(), root) != 0)
		return {};

	std::string relative = path.substr(root.size());
	while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\'))
		relative.erase(relative.begin());
	const size_t slash = relative.find_first_of("/\\");
	return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
}

struct MediaFolderAlias
{
	config::LibraryCoverMediaMode mode;
	std::vector<std::string_view> aliases;
};

std::string toLowerString(std::string_view value)
{
	std::string lower(value);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return lower;
}

const MediaFolderAlias* findMediaFolderAlias(const std::string& folder)
{
	static const std::array<MediaFolderAlias, 9> mediaAliases{
		MediaFolderAlias {config::LibraryCoverMediaMode::MixImage,
				{"miximages", "miximage", "miximg", "miximgs", "mix", "mixrbv1", "image", "images"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::Cover,
				{"covers", "cover", "box2dfront", "boxfront", "box-2d", "box2d"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::Case,
				{"3dboxes", "3dbox", "box3d", "backcovers", "backcover", "boxtexture", "box-texture", "boxtextures", "case", "cases", "insert", "custom"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::FanArt,
				{"fanart", "fanarts", "fan-art", "background", "backgrounds"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::Screenshot,
				{"screenshots", "screenshot", "ss"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::TitleScreen,
				{"titlescreens", "titlescreen", "title-screens", "title-screen", "sstitle"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::Title,
				{"marquees", "marquee", "wheelhd", "wheel-hd", "wheel", "wheels", "screenmarquee", "screenmarqueesmall", "title"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::Physical,
				{"physicalmedia", "physical-media", "support2d", "support-2d", "supporttexture", "support-texture", "support", "physical", "disc"}},
		MediaFolderAlias {config::LibraryCoverMediaMode::Manual,
				{"manuals", "manual", "manualss"}},
	};

	const std::string folderLower = toLowerString(folder);
	for (const MediaFolderAlias& aliasGroup : mediaAliases)
		for (const std::string_view alias : aliasGroup.aliases)
			if (folderLower == alias)
				return &aliasGroup;
	return nullptr;
}

bool isSupportedCustomMediaExtension(config::LibraryCoverMediaMode mediaMode, const std::string& ext)
{
	return mediaMode == config::LibraryCoverMediaMode::Manual
			? isSupportedManualExtension(ext)
			: isSupportedBoxartExtension(ext);
}

bool isGeneratedVmuIconPath(const std::string& root, const std::string& path)
{
	if (path.size() <= root.size() || path.compare(0, root.size(), root) != 0)
		return false;

	std::string relative = path.substr(root.size());
	while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\'))
		relative.erase(relative.begin());
	const size_t slash = relative.find_first_of("/\\");
	const std::string firstComponent = slash == std::string::npos ? relative : relative.substr(0, slash);
	return firstComponent == "vmu-icons";
}

} // namespace

GameBoxart Boxart::getBoxart(const GameMedia& media)
{
	loadDatabase();
	const int sourceMode = config::BoxartSourceMode.get();
	if (sourceMode == static_cast<int>(BoxartSourceMode::PhysicalMediaOnly))
		return getPhysicalBoxart(media);

	GameBoxart boxart;
	{
		std::lock_guard<std::mutex> guard(mutex);
		auto it = games.find(media.fileName);
		if (it != games.end())
			boxart = it->second;
		applyLibraryPlaytimeUnlocked(boxart);
	}
	if (boxart.fileName.empty())
		boxart = makeMediaBoxart(media);
	else if (isMissingBoxartFile(boxart))
		boxart.boxartPath.clear();

	if (sourceMode == static_cast<int>(BoxartSourceMode::CustomThenScraped))
	{
		const std::string customPath = getCustomBoxartPath(media);
		if (!customPath.empty())
			boxart.boxartPath = customPath;
	}
	// Original Box Art prefers scraped art, but still falls back to the disc's
	// physical media image when the online database has no artwork for the game.

	return boxart;
}

GameBoxart Boxart::getBoxartAndLoad(const GameMedia& media)
{
	return getBoxartAndQueue(media, true);
}

void Boxart::queueBoxart(const GameMedia& media)
{
	getBoxartAndQueue(media, false);
}

void Boxart::startFetch()
{
	fetchBoxart();
}

GameBoxart Boxart::getBoxartAndQueue(const GameMedia& media, bool startFetch)
{
	loadDatabase();
	const int sourceMode = config::BoxartSourceMode.get();
	if (sourceMode == static_cast<int>(BoxartSourceMode::PhysicalMediaOnly))
		return getPhysicalBoxart(media);
	const bool useCustomBoxart = sourceMode == static_cast<int>(BoxartSourceMode::CustomThenScraped);
	bool customIndexReady = true;
	std::string customPath;
	if (useCustomBoxart)
		customPath = getCustomBoxartPathForMediaMode(media, config::LibraryCoverMediaMode::CurrentArtwork, &customIndexReady);
	const bool queueScrape = !useCustomBoxart || (customIndexReady && customPath.empty());

	GameBoxart boxart;
	{
		std::lock_guard<std::mutex> guard(mutex);
		auto it = games.find(media.fileName);
		if (it != games.end())
		{
			boxart = it->second;
			const bool missingBoxartFile = isMissingBoxartFile(boxart);
			const bool missingScrapedBoxartFile = isMissingScrapedBoxartFile(boxart);
			const bool missingPhysicalBoxart = boxart.parsed && boxart.boxartPath.empty() && !boxart.scraped;
			if (missingBoxartFile || missingScrapedBoxartFile || missingPhysicalBoxart)
			{
				boxart.boxartPath.clear();
				if (missingBoxartFile || missingPhysicalBoxart)
					boxart.parsed = false;
				if (missingScrapedBoxartFile)
					boxart.scraped = false;
				it->second = boxart;
				markDatabaseDirty();
			}
			const bool wantOnline = shouldFetchOnline();
			const bool needsOffline = !boxart.parsed;
			const bool needsOnline = wantOnline && !boxart.scraped;
			if (queueScrape && !boxart.busy && (needsOffline || needsOnline))
			{
				boxart.busy = it->second.busy = true;
				boxart.gamePath = media.path;
				boxart.arcade = media.arcade;
				toFetch.push_back(boxart);
			}
		}
		else
		{
			boxart = makeMediaBoxart(media);
			boxart.busy = queueScrape;
			games[boxart.fileName] = boxart;
			if (queueScrape)
				toFetch.push_back(boxart);
		}
		applyLibraryPlaytimeUnlocked(boxart);
	}

	if (useCustomBoxart)
	{
		if (!customPath.empty())
		{
			boxart.boxartPath = customPath;
			boxart.busy = false;
		}
	}
	// Original Box Art keeps the physical media image visible while, or after,
	// online scraping finds metadata without an original boxart image.
	if (startFetch)
		fetchBoxart();

	return boxart;
}

bool Boxart::shouldFetchOnline() const
{
	return config::FetchBoxart && config::BoxartSourceMode.get() != static_cast<int>(BoxartSourceMode::PhysicalMediaOnly);
}

GameBoxart Boxart::getPhysicalBoxart(const GameMedia& media)
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		auto it = physicalCache.find(media.fileName);
		if (it != physicalCache.end())
		{
			GameBoxart boxart = it->second;
			applyLibraryPlaytimeUnlocked(boxart);
			return boxart;
		}
	}

	GameBoxart boxart = makeMediaBoxart(media);

	OfflineScraper physicalScraper;
	physicalScraper.initialize(getSaveDirectory());
	physicalScraper.scrape(boxart);

	{
		std::lock_guard<std::mutex> guard(mutex);
		physicalCache[boxart.fileName] = boxart;
		applyLibraryPlaytimeUnlocked(boxart);
	}

	return boxart;
}

std::string Boxart::getCustomBoxartPath(const GameMedia& media)
{
	return getCustomBoxartPathForMediaMode(media, config::LibraryCoverMediaMode::CurrentArtwork);
}

std::string Boxart::getLibraryCoverMediaPath(const GameMedia& media)
{
	if (config::BoxartSourceMode.get() != static_cast<int>(BoxartSourceMode::CustomThenScraped))
		return {};

	const auto coverMediaMode = static_cast<config::LibraryCoverMediaMode>(config::LibraryCoverMedia.get());
	if (coverMediaMode == config::LibraryCoverMediaMode::CurrentArtwork)
		return {};
	if (static_cast<size_t>(coverMediaMode) >= customBoxartByName.size())
		return {};
	return getCustomBoxartPathForMediaMode(media, coverMediaMode);
}

std::string Boxart::getCustomMediaPath(const GameMedia& media, config::LibraryCoverMediaMode mediaMode)
{
	return getCustomBoxartPathForMediaMode(media, mediaMode);
}

std::string Boxart::getCustomBoxartPathForMediaMode(const GameMedia& media, config::LibraryCoverMediaMode mediaMode,
		bool *indexReady)
{
	const size_t modeIndex = static_cast<size_t>(mediaMode);
	if (modeIndex >= customBoxartByName.size())
		return {};

	refreshCustomBoxartIndex(false);
	const std::string root = getSaveDirectory();

	const std::string fileKey = makeBoxartKey(media.fileName);
	const std::string nameKey = makeBoxartKey(media.name);
	const std::string gameNameKey = makeBoxartKey(media.gameName);

	std::lock_guard<std::mutex> guard(mutex);
	if (indexReady != nullptr)
		*indexReady = customIndexLoaded && customBoxartRoot == root;
	const auto& mediaIndex = customBoxartByName[modeIndex];
	if (!fileKey.empty())
	{
		auto it = mediaIndex.find(fileKey);
		if (it != mediaIndex.end())
			return it->second;
	}
	if (!nameKey.empty())
	{
		auto it = mediaIndex.find(nameKey);
		if (it != mediaIndex.end())
			return it->second;
	}
	if (!gameNameKey.empty())
	{
		auto it = mediaIndex.find(gameNameKey);
		if (it != mediaIndex.end())
			return it->second;
	}
	return {};
}

void Boxart::fetchBoxart()
{
	if (physicalFetching.valid() && physicalFetching.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			physicalFetching.get();
		} catch (const std::exception& e) {
			ERROR_LOG(COMMON, "Physical boxart scraper thread exception: %s", e.what());
		} catch (...) {
			ERROR_LOG(COMMON, "Physical boxart scraper thread unknown exception");
		}
	}
	if (onlineFetching.valid() && onlineFetching.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try {
			onlineFetching.get();
		} catch (const std::exception& e) {
			ERROR_LOG(COMMON, "Online boxart scraper thread exception: %s", e.what());
		} catch (...) {
			ERROR_LOG(COMMON, "Online boxart scraper thread unknown exception");
		}
	}

	if (!physicalFetching.valid())
	{
		bool hasPhysicalBoxart = false;
		{
			std::lock_guard<std::mutex> guard(mutex);
			for (const GameBoxart& item : toFetch)
				if (!item.parsed)
				{
					hasPhysicalBoxart = true;
					break;
				}
		}
		if (hasPhysicalBoxart)
		{
			physicalFetching = std::async(std::launch::async, [this]() {
				ThreadName _("BoxArt-physical");
				const bool wantOnline = shouldFetchOnline();
				while (true)
				{
					std::vector<GameBoxart> physicalBoxart;
					{
						std::lock_guard<std::mutex> guard(mutex);
						// Keep toFetch as a vector for online front-batching, but compact it
						// once here instead of erasing physical items one by one.
						std::vector<GameBoxart> remaining;
						remaining.reserve(toFetch.size());
						for (GameBoxart& item : toFetch)
						{
							if (!item.parsed && physicalBoxart.size() < kPhysicalBoxartBatchSize)
							{
								physicalBoxart.push_back(std::move(item));
							}
							else
							{
								remaining.push_back(std::move(item));
							}
						}
						toFetch = std::move(remaining);
					}
					if (physicalBoxart.empty())
						break;

					DEBUG_LOG(COMMON, "Scraping physical boxart for %d games", (int)physicalBoxart.size());
					scrapePhysicalBoxart(physicalBoxart, getSaveDirectory());
					{
						std::lock_guard<std::mutex> guard(mutex);
						for (GameBoxart& b : physicalBoxart)
							if (b.scraped || b.parsed)
							{
								auto existing = games.find(b.fileName);
								if (existing != games.end())
								{
									if (b.boxartPath.empty() && !existing->second.boxartPath.empty())
										b.boxartPath = existing->second.boxartPath;
									if (!existing->second.boxartUrl.empty())
										b.boxartUrl = existing->second.boxartUrl;
									if (existing->second.scraped)
										b.scraped = true;
								}
								if (wantOnline && !b.scraped)
									toFetch.push_back(b);
								else
									b.busy = false;
								games[b.fileName] = b;
								markDatabaseDirty();
							}
					}
					saveDatabase();
				}
			});
		}
	}

	if (physicalFetching.valid() || onlineFetching.valid())
		return;
	if (toFetch.empty())
		return;

	std::vector<GameBoxart> boxart;
	{
		std::lock_guard<std::mutex> guard(mutex);
		for (const GameBoxart& item : toFetch)
			if (!item.parsed)
				return;

		size_t size = std::min(toFetch.size(), kOnlineBoxartBatchSize);
		boxart = std::vector<GameBoxart>(toFetch.begin(), toFetch.begin() + size);
		toFetch.erase(toFetch.begin(), toFetch.begin() + size);
	}
	onlineFetching = std::async(std::launch::async, [this, boxart = std::move(boxart)]() mutable {
		ThreadName _("BoxArt-online");
		const bool wantOnline = shouldFetchOnline();
		DEBUG_LOG(COMMON, "Scraping online boxart for %d games", (int)boxart.size());
		if (wantOnline)
		{
			if (scraper == nullptr)
			{
				scraper = std::unique_ptr<Scraper>(new TheGamesDb());
				if (!scraper->initialize(getSaveDirectory()))
				{
					ERROR_LOG(COMMON, "thegamesdb scraper initialization failed");
					scraper.reset();
					std::lock_guard<std::mutex> guard(mutex);
					toFetch.insert(toFetch.begin(), boxart.begin(), boxart.end());
					return;
				}
			}
			if (arcadeScraper == nullptr) {
				arcadeScraper = std::make_unique<ArcadeScraper>();
				arcadeScraper->initialize(getSaveDirectory());
			}
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
					markDatabaseDirty();
				}
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
							markDatabaseDirty();
						}
						else
						{
							toFetch.push_back(b);
						}
				}
			}
		}
		else
		{
			std::lock_guard<std::mutex> guard(mutex);
			for (GameBoxart& b : boxart)
			{
				b.busy = false;
				games[b.fileName] = b;
			}
		}
		saveDatabase();
	});
}

std::string Boxart::getDatabaseDirectory() const
{
	const std::string artworkDir = getSaveDirectory();
	if (!isContentUri(artworkDir) && isWritableFilesystemDirectory(artworkDir))
		return artworkDir;

	// SAF and read-only artwork folders cannot provide atomic metadata writes. Keep
	// metadata in Hollycast's writable data area, keyed by the selected media root.
	const std::string boxartDir = get_writable_data_path("boxart/");
	if (!ensureFilesystemDirectory(boxartDir))
		return {};
	const std::string databaseRoot = appendFilesystemPath(boxartDir, "databases");
	if (!ensureFilesystemDirectory(databaseRoot))
		return {};
	const std::string databaseDir = appendFilesystemPath(databaseRoot, stableDatabaseKey(artworkDir));
	return ensureFilesystemDirectory(databaseDir) ? databaseDir : std::string{};
}

void Boxart::markDatabaseDirty()
{
	// All callers hold mutex. The generation protects changes made while a save is writing.
	databaseDirty = true;
	++databaseGeneration;
}

void Boxart::saveDatabase()
{
	std::vector<GameBoxart> snapshot;
	u64 savedGeneration = 0;
	const std::string artworkDir = getSaveDirectory();
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (!databaseDirty)
			return;
		savedGeneration = databaseGeneration;
		snapshot.reserve(games.size());
		for (const auto& game : games)
			if (game.second.scraped || game.second.parsed || !game.second.fileName.empty())
				snapshot.push_back(game.second);
	}

	const std::string databaseDir = getDatabaseDirectory();
	if (databaseDir.empty())
	{
		WARN_LOG(COMMON, "Can't save boxart database: no writable metadata directory for %s", artworkDir.c_str());
		return;
	}
	const std::string databasePath = appendFilesystemPath(databaseDir, DB_NAME);
	json array;
	for (const GameBoxart& game : snapshot)
		array.push_back(game.to_json(artworkDir));
	const std::string serialized = array.dump(4, ' ', false, json::error_handler_t::replace);

	if (!writeFileAtomically(databasePath, serialized))
	{
		WARN_LOG(COMMON, "Can't save boxart database to %s: error %d", databasePath.c_str(), errno);
		return;
	}

	std::lock_guard<std::mutex> guard(mutex);
	if (databaseGeneration == savedGeneration)
		databaseDirty = false;
}

void Boxart::recoverDatabases(const std::string& databaseDir, const std::string& artworkDir)
{
	std::vector<std::string> databasePaths;
	std::unordered_map<std::string, std::string> imagePathsByName;
	try {
		hostfs::DirectoryTree tree(databaseDir);
		for (auto it = tree.begin(); it != tree.end(); ++it)
		{
			const hostfs::FileInfo& entry = *it;
			if (entry.isDirectory)
				continue;
			if (entry.name == DB_NAME)
			{
				databasePaths.push_back(entry.path);
			}
		}
		hostfs::DirectoryTree artworkTree(artworkDir);
		for (auto it = artworkTree.begin(); it != artworkTree.end(); ++it)
		{
			const hostfs::FileInfo& entry = *it;
			if (!entry.isDirectory && isSupportedBoxartExtension(get_file_extension(entry.name)))
			{
				imagePathsByName.emplace(normalizePath(entry.name), entry.path);
				imagePathsByName.emplace(normalizePath(removeCopySuffix(entry.name)), entry.path);
			}
		}
	} catch (const std::exception& e) {
		WARN_LOG(COMMON, "Boxart database recovery scan failed: %s", e.what());
		return;
	}

	if (databasePaths.size() <= 1)
		return;

	const std::string mainDbPath = appendFilesystemPath(databaseDir, DB_NAME);
	std::unordered_map<std::string, GameBoxart> mergedGames;
	for (const std::string& dbPath : databasePaths)
	{
		std::string contents;
		if (!readStorageFile(dbPath, contents))
			continue;

		try {
			json v = json::parse(contents);
			if (!v.is_array())
				continue;
			const std::string dbDir = parentPath(dbPath);
			for (const auto& o : v)
			{
				GameBoxart candidate(o, dbDir);
				if (candidate.fileName.empty() && candidate.name.empty())
					continue;

				if (!candidate.boxartPath.empty() && !file_exists(candidate.boxartPath))
				{
					const std::string imageName = normalizePath(removeCopySuffix(fileNameFromPath(candidate.boxartPath)));
					auto imageIt = imagePathsByName.find(imageName);
					if (imageIt != imagePathsByName.end())
						candidate.boxartPath = imageIt->second;
				}
				if (candidate.boxartPath.empty() && !candidate.scraped)
					candidate.parsed = false;

				const std::string key = candidate.fileName.empty() ? candidate.name : candidate.fileName;
				auto existing = mergedGames.find(key);
				if (existing == mergedGames.end() || boxartMergeScore(candidate) > boxartMergeScore(existing->second))
					mergedGames[key] = candidate;
			}
		} catch (const json::exception& e) {
			WARN_LOG(COMMON, "Skipping corrupt boxart database %s: %s", dbPath.c_str(), e.what());
		}
	}

	if (mergedGames.empty())
		return;

	json array;
	for (const auto& game : mergedGames)
		array.push_back(game.second.to_json(artworkDir));

	if (!writeFileAtomically(mainDbPath, array.dump(4, ' ', false, json::error_handler_t::replace)))
	{
		WARN_LOG(COMMON, "Can't write recovered boxart database to %s: error %d", mainDbPath.c_str(), errno);
		return;
	}

	INFO_LOG(COMMON, "Recovered %d games from %d boxart databases without deleting source files", (int)mergedGames.size(), (int)databasePaths.size());
}

void Boxart::loadDatabase()
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (databaseLoaded)
			return;
		databaseLoaded = true;
		databaseDirty = false;
		++databaseGeneration;
	}

	const std::string artworkDir = getSaveDirectory();
	loadLibraryPlaytimeDatabase();
	const std::string databaseDir = getDatabaseDirectory();
	if (databaseDir.empty())
	{
		WARN_LOG(COMMON, "Can't load boxart database: no writable metadata directory for %s", artworkDir.c_str());
		refreshCustomBoxartIndex(false);
		return;
	}
	if (databaseDir == artworkDir)
		recoverDatabases(databaseDir, artworkDir);

	std::string contents;
	std::string databasePath = appendFilesystemPath(databaseDir, DB_NAME);
	bool loadedLegacyDatabase = false;
	if (!readStorageFile(databasePath, contents) && databaseDir != artworkDir)
	{
		try {
			const std::string legacyPath = isContentUri(artworkDir)
				? hostfs::storage().getSubPath(artworkDir, DB_NAME)
				: appendFilesystemPath(artworkDir, DB_NAME);
			loadedLegacyDatabase = readStorageFile(legacyPath, contents);
		} catch (const hostfs::StorageException&) {
		}
	}
	if (contents.empty())
	{
		refreshCustomBoxartIndex(false);
		return;
	}

	DEBUG_LOG(COMMON, "Loading boxart database from %s", databasePath.c_str());
	try {
		std::vector<GameBoxart> loadedGames;
		json v = json::parse(contents);
		for (const auto& o : v)
			loadedGames.emplace_back(o, artworkDir);
		std::lock_guard<std::mutex> guard(mutex);
		for (GameBoxart& game : loadedGames)
			games[game.fileName] = std::move(game);
		if (loadedLegacyDatabase)
			markDatabaseDirty();
	} catch (const json::exception& e) {
		WARN_LOG(COMMON, "Corrupted database file: %s", e.what());
	}
	refreshCustomBoxartIndex(false);
}

void Boxart::applyLibraryPlaytimeUnlocked(GameBoxart& boxart) const
{
	boxart.playTimeSeconds.reset();
	const std::string gameId = normalizeLibraryGameId(boxart.uniqueId);
	if (gameId.empty())
		return;
	const auto it = libraryPlaytimeByGameId.find(gameId);
	if (it != libraryPlaytimeByGameId.end())
		boxart.playTimeSeconds = it->second;
}

void Boxart::loadLibraryPlaytimeDatabase()
{
	std::unordered_map<std::string, u64> loadedPlaytimes;
	const std::string databasePath = getLibraryPlaytimeDatabasePath(getSaveDirectory());
	if (databasePath.empty())
	{
		std::lock_guard<std::mutex> guard(mutex);
		libraryPlaytimeByGameId.clear();
		return;
	}

	try {
		const hostfs::FileInfo info = hostfs::storage().getFileInfo(databasePath);
		if (info.isDirectory || info.size > kLibraryPlaytimeDatabaseMaxSize)
		{
			WARN_LOG(COMMON, "Ignoring Library play-time database %s: invalid file type or size", databasePath.c_str());
			std::lock_guard<std::mutex> guard(mutex);
			libraryPlaytimeByGameId.clear();
			return;
		}
	} catch (const hostfs::StorageException&) {
		std::lock_guard<std::mutex> guard(mutex);
		libraryPlaytimeByGameId.clear();
		return;
	}

	std::string contents;
	if (!readStorageFile(databasePath, contents, kLibraryPlaytimeDatabaseMaxSize))
	{
		WARN_LOG(COMMON, "Can't read Library play-time database %s", databasePath.c_str());
		std::lock_guard<std::mutex> guard(mutex);
		libraryPlaytimeByGameId.clear();
		return;
	}

	try {
		const json root = json::parse(contents);
		if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("games") || !root["games"].is_object())
			throw json::type_error::create(302, "expected a version 1 Library play-time database", &root);
		const json& games = root["games"];
		if (games.size() > kLibraryPlaytimeDatabaseMaxEntries)
			throw json::out_of_range::create(408, "too many Library play-time entries", &games);

		size_t invalidEntries = 0;
		for (auto it = games.begin(); it != games.end(); ++it)
		{
			const std::string gameId = normalizeLibraryGameId(it.key());
			const json& entry = it.value();
			auto seconds = entry.is_object() ? entry.find("seconds") : entry.end();
			if (gameId.empty() || gameId.size() > 64 || seconds == entry.end()
					|| (!seconds->is_number_unsigned() && !seconds->is_number_integer())
					|| (seconds->is_number_integer() && seconds->get<s64>() < 0))
			{
				++invalidEntries;
				continue;
			}
			loadedPlaytimes.emplace(gameId, seconds->get<u64>());
		}
		if (invalidEntries != 0)
			WARN_LOG(COMMON, "Ignored %d invalid Library play-time entries in %s", (int)invalidEntries, databasePath.c_str());
	} catch (const json::exception& e) {
		WARN_LOG(COMMON, "Ignoring invalid Library play-time database %s: %s", databasePath.c_str(), e.what());
		loadedPlaytimes.clear();
	}

	std::lock_guard<std::mutex> guard(mutex);
	libraryPlaytimeByGameId = std::move(loadedPlaytimes);
}

void Boxart::refreshLibraryPlaytimeDatabase()
{
	loadLibraryPlaytimeDatabase();
}

void Boxart::reviewDatabaseArtwork()
{
	std::lock_guard<std::mutex> guard(mutex);
	for (auto& game : games)
	{
		if (!game.second.boxartPath.empty())
		{
			const bool usableImage = isSupportedBoxartExtension(get_file_extension(game.second.boxartPath)) && file_exists(game.second.boxartPath);
			if (usableImage)
				continue;
			game.second.boxartPath.clear();
			if (!game.second.boxartUrl.empty())
				game.second.scraped = false;
			markDatabaseDirty();
		}

		if (game.second.parsed && game.second.boxartPath.empty() && !game.second.scraped)
		{
			game.second.parsed = false;
			game.second.busy = false;
			markDatabaseDirty();
		}
		if (game.second.boxartPath.empty() && game.second.scraped)
		{
			game.second.scraped = false;
			game.second.busy = false;
			markDatabaseDirty();
		}
	}
}

void Boxart::refreshCache()
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		customIndexShuttingDown = true;
		++customIndexGeneration;
	}
	if (customIndexFetching.valid())
		customIndexFetching.get();
	{
		std::lock_guard<std::mutex> guard(mutex);
		customIndexShuttingDown = false;
		customIndexState = CustomIndexState::NotRequested;
	}
	if (physicalFetching.valid())
	{
		try {
			physicalFetching.get();
		} catch (const std::exception& e) {
			ERROR_LOG(COMMON, "Physical boxart scraper thread exception: %s", e.what());
		} catch (...) {
			ERROR_LOG(COMMON, "Physical boxart scraper thread unknown exception");
		}
	}
	if (onlineFetching.valid())
	{
		try {
			onlineFetching.get();
		} catch (const std::exception& e) {
			ERROR_LOG(COMMON, "Online boxart scraper thread exception: %s", e.what());
		} catch (...) {
			ERROR_LOG(COMMON, "Online boxart scraper thread unknown exception");
		}
	}
	saveDatabase();
	{
		std::lock_guard<std::mutex> guard(mutex);
		games.clear();
		physicalCache.clear();
		libraryPlaytimeByGameId.clear();
		toFetch.clear();
		databaseLoaded = false;
		databaseDirty = false;
		++databaseGeneration;
		customIndexLoaded = false;
		customIndexState = CustomIndexState::NotRequested;
		requestedCustomBoxartRoot.clear();
	}
	loadDatabase();
	reviewDatabaseArtwork();
	refreshCustomBoxartIndex(true);
	saveDatabase();
}

void Boxart::term()
{
	{
		std::lock_guard<std::mutex> guard(mutex);
		customIndexShuttingDown = true;
		++customIndexGeneration;
	}
	if (customIndexFetching.valid())
		customIndexFetching.get();
	if (physicalFetching.valid())
		physicalFetching.get();
	if (onlineFetching.valid())
		onlineFetching.get();
}

void Boxart::refreshCustomBoxartIndex(bool force)
{
	const std::string root = getSaveDirectory();

	if (customIndexFetching.valid() && customIndexFetching.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		customIndexFetching.get();

	bool startWorker = false;
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (customIndexShuttingDown)
			return;

		const bool rootChanged = root != requestedCustomBoxartRoot;
		const bool needsInitialBuild = customIndexState == CustomIndexState::NotRequested;
		if (!rootChanged && !force && !needsInitialBuild)
			return;

		if (rootChanged)
		{
			requestedCustomBoxartRoot = root;
			if (root != customBoxartRoot)
			{
				for (auto& mediaIndex : customBoxartByName)
					mediaIndex.clear();
				customBoxartRoot = root;
				customIndexLoaded = false;
				physicalCache.clear();
			}
		}
		++customIndexGeneration;
		customIndexState = CustomIndexState::Building;
		startWorker = !customIndexFetching.valid();
	}

	if (startWorker)
		customIndexFetching = std::async(std::launch::async, [this]() { buildCustomBoxartIndex(); });
}

void Boxart::buildCustomBoxartIndex()
{
	while (true)
	{
		std::string root;
		u64 generation;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (customIndexShuttingDown)
				return;
			root = requestedCustomBoxartRoot;
			generation = customIndexGeneration;
		}

		std::unordered_map<std::string, std::string> newIndex;
		CustomBoxartIndex mediaModeIndexes;
		bool succeeded = true;
		try {
			if (!root.empty())
			{
				hostfs::storage().getFileInfo(root);
				hostfs::DirectoryTree tree(root);
				for (auto it = tree.begin(); it != tree.end(); ++it)
				{
					const hostfs::FileInfo& entry = *it;
					if (entry.isDirectory || isGeneratedVmuIconPath(root, entry.path))
						continue;
					const std::string ext = get_file_extension(entry.name);
					const std::string key = makeBoxartKey(entry.name);
					if (key.empty())
						continue;

					const std::string firstFolder = toLowerString(getParentPath(root, entry.path));
					const MediaFolderAlias* alias = findMediaFolderAlias(firstFolder);
					if (alias == nullptr)
					{
						if (isSupportedBoxartExtension(ext))
							newIndex[key] = entry.path;
						continue;
					}
					if (!isSupportedCustomMediaExtension(alias->mode, ext))
						continue;
					if (isSupportedBoxartExtension(ext))
						newIndex[key] = entry.path;

					const size_t modeIndex = static_cast<size_t>(alias->mode);
					if (mediaModeIndexes[modeIndex].find(key) == mediaModeIndexes[modeIndex].end())
						mediaModeIndexes[modeIndex][key] = entry.path;
				}
			}
		} catch (const std::exception& e) {
			WARN_LOG(COMMON, "Custom boxart scan failed: %s", e.what());
			succeeded = false;
		}

		std::lock_guard<std::mutex> guard(mutex);
		if (customIndexShuttingDown)
			return;
		if (generation != customIndexGeneration)
			continue;
		if (!succeeded)
		{
			if (!customIndexLoaded || customBoxartRoot != root)
			{
				for (auto& mediaIndex : customBoxartByName)
					mediaIndex.clear();
				customBoxartRoot = root;
				customIndexLoaded = false;
			}
			customIndexState = CustomIndexState::Failed;
			return;
		}

		customBoxartRoot = root;
		for (size_t i = 0; i < customBoxartByName.size(); ++i)
			customBoxartByName[i].swap(mediaModeIndexes[i]);
		customBoxartByName[static_cast<size_t>(config::LibraryCoverMediaMode::CurrentArtwork)] = std::move(newIndex);
		customIndexLoaded = true;
		customIndexState = CustomIndexState::Ready;
		return;
	}
}
