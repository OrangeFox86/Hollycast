/*
	Copyright 2024 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "gui_util.h"
#include "types.h"
#include "stdclass.h"
#include "oslib/oslib.h"
#include "oslib/directory.h"
#include "oslib/storage.h"
#include "oslib/http_client.h"
#include "oslib/i18n.h"
#include "gui_menu.h"
#include "imgui_driver.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "gui_font.h"
#include "IconsFontAwesome6.h"
#include "stdclass.h"
#include "rend/osd.h"
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <vector>

using namespace i18n;

static std::string select_current_directory = "**home**";
static std::vector<hostfs::FileInfo> subfolders;
static std::vector<hostfs::FileInfo> folderFiles;
bool subfolders_read;
static char fileInputText[1024] = {};
static bool fileInputIsDirectory;
static char filePathText[1024] = {};
static char fileSearchText[128] = {};
static std::vector<std::string> select_back_history;
static std::vector<std::string> select_forward_history;
static bool select_pathbox_text_mode;
static std::mutex g_storageCallbackMutex;
static StringCallback g_storageCallback;

extern int insetLeft, insetRight, insetTop, insetBottom;
void error_popup();

static void storage_popup_callback(bool cancelled, std::string selectedPath)
{
	StringCallback callback;

	{
		std::lock_guard<std::mutex> lock(g_storageCallbackMutex);
		callback = g_storageCallback;
		g_storageCallback = {};
	}

	if (callback)
		callback(cancelled, selectedPath);
}

static ImFont* SettingsDescriptionFont()
{
	return settingsTitleFont != nullptr ? settingsTitleFont : largeFont;
}

static ImFont* PopupEmphasisFont()
{
	return settingsValueFont != nullptr ? settingsValueFont : largeFont;
}

static ImFont* SettingsRightValueFont()
{
	return settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont;
}

namespace hostfs
{
	bool operator<(const FileInfo& a, const FileInfo& b) {
		return locale()(a.name, b.name);
	}
}

void select_file_popup(const char *prompt, const StringCallback& callback,
		bool selectFile, const std::string& selectExtension)
{
	const float windowInset = uiScaled(8.0f);
	const ImVec2 parentPos = ImGui::GetWindowPos();
	const ImVec2 parentSize = ImGui::GetWindowSize();
	const ImVec2 popupPos(parentPos.x + windowInset, parentPos.y + windowInset);
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const ImVec2 desiredSize(parentSize.x - windowInset * 2.0f, parentSize.y - windowInset * 2.0f);
	const ImVec2 minSize(uiScaled(320.0f), uiScaled(240.0f));
	const ImVec2 maxSize(
			std::max(minSize.x, displaySize.x - popupPos.x - windowInset),
			std::max(minSize.y, displaySize.y - popupPos.y - windowInset));
	ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImClamp(desiredSize, minSize, maxSize), ImGuiCond_Always);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_FramePadding, ImVec2(4, 3)); // default

	if (ImGui::BeginPopup(prompt, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
			| ImGuiWindowFlags_NoSavedSettings))
	{
		static std::string error_message;

		if (select_current_directory == "**home**")
			select_current_directory = hostfs::storage().getDefaultDirectory();

		auto setInputText = [](const std::string& text) {
			std::strncpy(fileInputText, text.c_str(), sizeof(fileInputText) - 1);
			fileInputText[sizeof(fileInputText) - 1] = '\0';
		};

		auto setPathText = [](const std::string& text) {
			std::strncpy(filePathText, text.c_str(), sizeof(filePathText) - 1);
			filePathText[sizeof(filePathText) - 1] = '\0';
		};

		auto setCurrentFolderInput = [&]() {
			if (selectFile)
				fileInputText[0] = '\0';
			else
				setInputText(select_current_directory);
			fileInputIsDirectory = false;
			setPathText(select_current_directory);
		};

		auto navigateToDirectory = [&](const std::string& path, bool addToHistory) {
			if (path == select_current_directory)
				return;
			if (addToHistory && !select_current_directory.empty())
			{
				select_back_history.push_back(select_current_directory);
				select_forward_history.clear();
			}
			select_current_directory = path;
			subfolders_read = false;
			setCurrentFolderInput();
		};

		if (ImGui::IsWindowAppearing())
		{
			fileSearchText[0] = '\0';
			select_back_history.clear();
			select_forward_history.clear();
			select_pathbox_text_mode = false;
			setCurrentFolderInput();
		}

		auto finalizeSelection = [&](const std::string& selection) {
			bool success = false;
			if (selectFile)
			{
				if (!selection.empty())
				{
					const std::string path = isAbsolutePath(selection)
							? selection : hostfs::storage().getSubPath(select_current_directory, selection);
					if (!fileInputIsDirectory)
						success = callback && callback(false, path);
				}
			}
			else
			{
				std::string path = select_current_directory;
				if (!selection.empty())
					path = isAbsolutePath(selection)
							? selection : hostfs::storage().getSubPath(select_current_directory, selection);
				success = callback && callback(false, path);
			}

#if defined(_WIN32) && !defined(TARGET_UWP)
			if (!success)
				MessageBeep(MB_ICONERROR);
#endif
			if (success)
			{
				subfolders_read = false;
				ImGui::CloseCurrentPopup();
			}
		};

		if (!subfolders_read)
		{
			subfolders.clear();
            folderFiles.clear();
			error_message.clear();

			auto isSupportedGameExtension = [](const std::string& extension) {
				return extension == "zip" || extension == "7z" || extension == "chd"
						|| extension == "gdi" || extension == "cdi" || extension == "cue"
						|| (!config::HideLegacyNaomiRoms
								&& (extension == "bin" || extension == "lst" || extension == "dat"));
			};

			try {
				for (const hostfs::FileInfo& entry : hostfs::storage().listContent(select_current_directory))
				{
					if (entry.isDirectory)
					{
						subfolders.push_back(entry);
					}
					else
					{
						std::string extension = get_file_extension(entry.name);
						if (selectFile)
						{
							if ((selectExtension.empty() && isSupportedGameExtension(extension))
									|| extension == selectExtension)
								folderFiles.push_back(entry);
						}
						else
							folderFiles.push_back(entry);
					}
				}
			} catch (const hostfs::StorageException& e) {
				error_message = e.what();
			}

			std::sort(subfolders.begin(), subfolders.end());
			std::sort(folderFiles.begin(), folderFiles.end());
			subfolders_read = true;
		}
		if (prompt != nullptr) {
			ImguiStyleVar _(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)); // Left
			ImguiStyleVar _1(ImGuiStyleVar_DisabledAlpha, 1.0f);
			ImGui::BeginDisabled();
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::ButtonEx(prompt, ImVec2(-1, 0));
			ImGui::PopFont();
			ImGui::EndDisabled();
		}
		auto matchesSearch = [](const std::string& name) {
			if (fileSearchText[0] == '\0')
				return true;

			std::string haystack = name;
			std::string needle = fileSearchText;
			std::transform(haystack.begin(), haystack.end(), haystack.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::transform(needle.begin(), needle.end(), needle.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return haystack.find(needle) != std::string::npos;
		};

		const bool showError = !error_message.empty();
		if (showError)
			ImGui::TextUnformatted(error_message.c_str());

		// POSIX allows '\' inside filenames, so only treat it as a separator when
		// the current build is targeting Windows paths.
		auto isPathSeparator = [](char c) {
#ifdef _WIN32
			return c == '/' || c == '\\';
#else
			return c == '/';
#endif
		};

		auto trimTrailingPathSeparators = [&](std::string path) {
			while (path.size() > 1 && isPathSeparator(path.back()))
				path.pop_back();
			return path;
		};

		auto findLastPathSeparator = [&](const std::string& path) {
			for (size_t i = path.size(); i-- > 0;)
				if (isPathSeparator(path[i]))
					return i;
			return std::string::npos;
		};

		auto getPathLeaf = [&](const std::string& path) {
			if (path.empty())
				return std::string("/");
			std::string normalized = trimTrailingPathSeparators(path);
			const size_t pos = findLastPathSeparator(normalized);
			if (pos == std::string::npos)
				return normalized;
			return normalized.substr(pos + 1);
		};

		auto getNavigationChain = [](const std::string& startPath) {
			std::vector<std::string> chain;
			if (startPath.empty())
				return chain;

			std::string cursor = startPath;
			for (int guard = 0; guard < 64 && !cursor.empty(); ++guard)
			{
				if (chain.empty() || chain.back() != cursor)
					chain.push_back(cursor);

				std::string parent = hostfs::storage().getParentPath(cursor);
				if (parent.empty() || parent == cursor)
					break;
				cursor = parent;
			}
			std::reverse(chain.begin(), chain.end());
			return chain;
		};

		auto normalizePathForCompare = [&](std::string path) {
			path = trimTrailingPathSeparators(std::move(path));
#ifdef _WIN32
			std::transform(path.begin(), path.end(), path.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
			return path;
		};

		auto isSamePath = [&](const std::string& lhs, const std::string& rhs) {
			return normalizePathForCompare(lhs) == normalizePathForCompare(rhs);
		};

		const float navSize = ImGui::GetFrameHeight();
		auto formatTimestamp = [](u64 updateTime) {
			if (updateTime == 0)
				return std::string("---");
			std::time_t rawTime = static_cast<std::time_t>(updateTime);
			std::tm timeInfo {};
#ifdef _WIN32
			if (localtime_s(&timeInfo, &rawTime) != 0)
				return std::string("---");
#else
			if (localtime_r(&rawTime, &timeInfo) == nullptr)
				return std::string("---");
#endif
			char buffer[32];
			std::strftime(buffer, sizeof(buffer), "%m/%d/%Y %H:%M", &timeInfo);
			return std::string(buffer);
		};

		auto FavoriteButton = [&](bool enabled) {
			if (!enabled)
			{
				ImGui::BeginDisabled(true);
				ImGui::PushFont(settingsIconFont);
				ImGui::Button(ICON_FA_STAR, ImVec2(navSize, navSize));
				ImGui::PopFont();
				ImGui::EndDisabled();
				return false;
			}
			ImGui::PushFont(settingsIconFont);
			const bool pressed = ImGui::Button(ICON_FA_STAR, ImVec2(navSize, navSize));
			ImGui::PopFont();
			return pressed;
		};

		auto PathBox = [&](bool* editMode) {
			if (*editMode)
			{
				const bool submitted = ImGui::InputTextEx("##pathbox", T("Path"), filePathText, sizeof(filePathText),
						ImVec2(-uiScaled(220), navSize), ImGuiInputTextFlags_EnterReturnsTrue);
				if (submitted)
				{
					navigateToDirectory(filePathText, true);
					*editMode = false;
				}
				else if (ImGui::IsItemDeactivated())
					*editMode = false;
				return;
			}

			ImGui::BeginChild("##pathbox", ImVec2(-uiScaled(220), navSize),
					ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
			{
				const std::vector<std::string> chain = getNavigationChain(select_current_directory);
				if (ImGui::SmallButton(T("This PC")))
					navigateToDirectory("", true);
				bool first = true;
				for (size_t i = 0; i < chain.size(); ++i)
				{
					const std::string leaf = chain[i] == "/" ? "/" : getPathLeaf(chain[i]);
					if (first)
					{
						ImGui::SameLine(0.0f, 2.0f);
						ImGui::Text(">");
						ImGui::SameLine(0.0f, 2.0f);
					}
					if (!first)
					{
						ImGui::SameLine(0.0f, 2.0f);
						ImGui::Text(">");
						ImGui::SameLine(0.0f, 2.0f);
					}
					first = false;

					if (ImGui::SmallButton(leaf.c_str()))
						navigateToDirectory(chain[i], true);
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
					{
						setPathText(chain[i]);
						fileInputIsDirectory = true;
						*editMode = true;
					}
				}
			}
			ImGui::EndChild();
		};

		auto drawBrowserItemLabel = [&](const char* icon, const std::string& name, const ImVec2& pos,
				bool disabled, float indent) {
			ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : ImGui::GetFont();
			ImFont* textFont = ImGui::GetFont();
			ImGuiStyle& style = ImGui::GetStyle();
			const float textHeight = ImGui::GetTextLineHeight();
			const float iconSize = ImGui::GetFontSize();
			const float iconWidth = uiScaled(18.0f);
			const ImU32 color = ImGui::GetColorU32(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
			const float itemX = pos.x + style.FramePadding.x + indent;
			const ImVec2 iconPos(itemX, pos.y + style.FramePadding.y);
			const ImVec2 textPos(itemX + iconWidth, pos.y + style.FramePadding.y);

			ImGui::GetWindowDrawList()->AddText(iconFont, iconSize, iconPos, color, icon);
			ImGui::GetWindowDrawList()->AddText(textFont, textHeight, textPos, color, name.c_str());
		};

		auto FolderNode = [&](const hostfs::FileInfo& entry, bool inContentPane, int treeDepth) {
			const bool selected = inContentPane
					? fileInputIsDirectory && std::strcmp(fileInputText, entry.path.c_str()) == 0
					: isSamePath(select_current_directory, entry.path);
			const char* icon = selected ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER;
			const ImVec2 rowSize(0.0f, ImGui::GetTextLineHeightWithSpacing());
			if (inContentPane)
				ImGui::TableNextRow();
			if (inContentPane)
				ImGui::TableSetColumnIndex(0);
			ImGui::PushID(entry.path.empty() ? entry.name.c_str() : entry.path.c_str());
			const ImVec2 rowPos = ImGui::GetCursorScreenPos();
			if (ImGui::Selectable("##folder", selected,
					inContentPane
							? (ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)
							: 0,
					rowSize))
			{
				setInputText(entry.path);
				fileInputIsDirectory = true;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					navigateToDirectory(entry.path, true);
				else if (!inContentPane)
					navigateToDirectory(entry.path, true);
			}
			drawBrowserItemLabel(icon, entry.name, rowPos, false,
					inContentPane ? 0.0f : uiScaled(14.0f) * treeDepth);
			ImGui::PopID();
			if (inContentPane)
			{
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(formatTimestamp(entry.updateTime).c_str());

				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted("");
			}
		};

		auto FileNode = [&](const hostfs::FileInfo& entry) {
			const bool selected = !fileInputIsDirectory && std::strcmp(fileInputText, entry.name.c_str()) == 0;
			const ImVec2 rowSize(0.0f, ImGui::GetTextLineHeightWithSpacing());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(entry.name.c_str());
			const ImVec2 rowPos = ImGui::GetCursorScreenPos();
			if (ImGui::Selectable("##file", selected,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
					rowSize))
			{
				setInputText(entry.name);
				fileInputIsDirectory = false;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					finalizeSelection(entry.name);
			}
			drawBrowserItemLabel(ICON_FA_FILE, entry.name, rowPos, false, 0.0f);
			ImGui::PopID();

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(formatTimestamp(entry.updateTime).c_str());

			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%.3f KiB", entry.size / 1024.0f);
		};

		ImGui::Separator();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		const bool canGoBack = !select_back_history.empty();
		if (!canGoBack)
			ImGui::BeginDisabled(true);
		if (ImGui::ArrowButton("##back", ImGuiDir_Left) && canGoBack)
		{
			select_forward_history.push_back(select_current_directory);
			const std::string path = select_back_history.back();
			select_back_history.pop_back();
			navigateToDirectory(path, false);
		}
		if (!canGoBack)
			ImGui::EndDisabled();
		ImGui::SameLine();
		const bool canGoForward = !select_forward_history.empty();
		if (!canGoForward)
			ImGui::BeginDisabled(true);
		if (ImGui::ArrowButton("##forward", ImGuiDir_Right) && canGoForward)
		{
			select_back_history.push_back(select_current_directory);
			const std::string path = select_forward_history.back();
			select_forward_history.pop_back();
			navigateToDirectory(path, false);
		}
		if (!canGoForward)
			ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::ArrowButton("##up", ImGuiDir_Up)
				&& !select_current_directory.empty() && select_current_directory != "/")
		{
			std::string parent = hostfs::storage().getParentPath(select_current_directory);
			if (parent == select_current_directory)
				parent.clear();
			navigateToDirectory(parent, true);
		}
		ImGui::SameLine();
		PathBox(&select_pathbox_text_mode);
		ImGui::SameLine();
		FavoriteButton(false);
		ImGui::SameLine();
		if (ImGui::InputTextEx("##searchTB", T("Search"), fileSearchText, sizeof(fileSearchText),
				ImVec2(-FLT_MIN, navSize), 0))
		{
		}
		ImGui::Separator();

		const ImGuiStyle& style = ImGui::GetStyle();
		const float bottomControlsHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f
				+ style.ItemSpacing.y * 2.0f
				+ style.WindowPadding.y;
		const float browserHeight = std::max(uiScaled(120.0f),
				ImGui::GetContentRegionAvail().y - bottomControlsHeight);
		ImGui::BeginChild("##file_browser_region", ImVec2(0, browserHeight),
				ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (ImGui::BeginTable("##file_dialog_layout", 2, ImGuiTableFlags_Resizable, ImVec2(0, 0)))
		{
			ImGui::TableSetupColumn("##tree", ImGuiTableColumnFlags_WidthFixed, uiScaled(170));
			ImGui::TableSetupColumn("##content", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::BeginChild("##treeContainer", ImVec2(0, 0),
					ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
			{
				ImguiStyleVar _(ImGuiStyleVar_ItemSpacing, ScaledVec2(8, 8));
				ImGui::TextDisabled("%s", T("Folders"));
				ImGui::Separator();

				hostfs::FileInfo rootEntry;
				rootEntry.name = T("This device");
				rootEntry.path = "";
				FolderNode(rootEntry, false, 0);

				std::vector<hostfs::FileInfo> roots;
				try {
					roots = hostfs::storage().listContent("");
					std::sort(roots.begin(), roots.end());
				} catch (const hostfs::StorageException&) {
				}

				const std::vector<std::string> navigationChain = getNavigationChain(select_current_directory);
				const std::string activeRoot = navigationChain.empty() ? std::string() : navigationChain.front();
				for (const auto& root : roots)
				{
					if (!root.isDirectory || !matchesSearch(root.name))
						continue;
					FolderNode(root, false, 1);

					if (!activeRoot.empty() && isSamePath(root.path, activeRoot))
					{
						for (size_t i = 1; i < navigationChain.size(); ++i)
						{
							const std::string& path = navigationChain[i];
							const std::string leafName = getPathLeaf(path);
							const std::string label = leafName.empty() ? path : leafName;
							hostfs::FileInfo breadcrumb;
							breadcrumb.name = label;
							breadcrumb.path = path;
							FolderNode(breadcrumb, false, static_cast<int>(i + 1));
						}
						for (const auto& entry : subfolders)
						{
							if (!matchesSearch(entry.name))
								continue;

							FolderNode(entry, false, static_cast<int>(navigationChain.size() + 1));
						}
					}
				}

				if (roots.empty())
				{
					for (const auto& entry : subfolders)
					{
						if (!matchesSearch(entry.name))
							continue;

						FolderNode(entry, false, 1);
					}
				}
				scrollWhenDraggingOnVoid();
				windowDragScroll();
			}
			ImGui::EndChild();

			ImGui::TableSetColumnIndex(1);
			ImGui::BeginChild("##contentContainer", ImVec2(0, 0),
					ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
			{
				if (ImGui::BeginTable("##contentTable", 3,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
				{
					ImGui::TableSetupColumn(T("Name"), ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn(T("Date modified"), ImGuiTableColumnFlags_WidthFixed, uiScaled(170));
					ImGui::TableSetupColumn(T("Size"), ImGuiTableColumnFlags_WidthFixed, uiScaled(95));
					ImGui::TableHeadersRow();

					for (const auto& entry : subfolders)
					{
						if (!matchesSearch(entry.name))
							continue;
						FolderNode(entry, true, 0);
					}

					for (const auto& entry : folderFiles)
					{
						if (!matchesSearch(entry.name))
							continue;
						if (selectFile)
							FileNode(entry);
						else {
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							drawBrowserItemLabel(ICON_FA_FILE, entry.name, ImGui::GetCursorScreenPos(), true, 0.0f);
							ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()));
							ImGui::TableSetColumnIndex(1);
							ImGui::TextUnformatted(formatTimestamp(entry.updateTime).c_str());
							ImGui::TableSetColumnIndex(2);
							ImGui::Text("%.3f KiB", entry.size / 1024.0f);
						}
					}

					ImGui::EndTable();
				}
				scrollWhenDraggingOnVoid();
				windowDragScroll();
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}
		ImGui::EndChild();
		ImGui::TextUnformatted(selectFile ? T("File name:") : T("Folder:"));
		ImGui::SameLine();
		const bool inputSubmitted = ImGui::InputTextEx("##file_input", T("Filename"), fileInputText, sizeof(fileInputText),
				ImVec2(-FLT_MIN, 0),
				ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsItemEdited())
			fileInputIsDirectory = false;
		if (inputSubmitted)
			finalizeSelection(fileInputText);
		const float buttonWidth = std::max(ImGui::CalcTextSize(T("Open")).x, ImGui::CalcTextSize(T("Back")).x)
				+ ImGui::GetStyle().FramePadding.x * 2.f + uiScaled(32.f);
		const float ok_cancel_width = buttonWidth * 2.f + ImGui::GetStyle().ItemSpacing.x;
		ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ok_cancel_width);
		if (ImGui::Button(T("Open"),
				ImVec2(buttonWidth, 0.0f)))
		{
			finalizeSelection(fileInputText);
		}
		ImGui::SameLine();
		if (ImGui::Button(T("Back"), ImVec2(-FLT_MIN, 0.0f)))
		{
			subfolders_read = false;
			if (callback)
				callback(true, "");
			ImGui::CloseCurrentPopup();
		}
		error_popup();
		ImGui::PopStyleColor();
		ImGui::EndPopup();
	}
}

StoragePopupResult select_storage_popup(bool isDirectory, bool writeAccess, const std::string& description,
		const StringCallback& callback, const std::string& mimeType)
{
	{
		std::lock_guard<std::mutex> lock(g_storageCallbackMutex);
		if (g_storageCallback)
			return StoragePopupResult::CallbackAlreadySet;
		g_storageCallback = callback;
	}

	const bool supported = hostfs::addStorage(isDirectory, writeAccess, description, &storage_popup_callback, mimeType);
	if (!supported)
	{
		std::lock_guard<std::mutex> lock(g_storageCallbackMutex);
		g_storageCallback = {};
	}
	return supported ? StoragePopupResult::Supported : StoragePopupResult::Unsupported;
}

// See https://github.com/ocornut/imgui/issues/3379
void scrollWhenDraggingOnVoid(ImGuiMouseButton mouse_button)
{
	ImGuiContext& g = *ImGui::GetCurrentContext();
	ImGuiWindow* window = g.CurrentWindow;
	while (window != nullptr
			&& (window->Flags & ImGuiWindowFlags_ChildWindow)
			&& !(window->Flags & ImGuiWindowFlags_DragScrolling)
			&& window->ScrollMax.x == 0.0f
			&& window->ScrollMax.y == 0.0f)
		window = window->ParentWindow;
	if (window == nullptr || !(window->Flags & ImGuiWindowFlags_DragScrolling))
		return;
#if defined(__ANDROID__)
	const ImGuiIO& io = ImGui::GetIO();
	const bool touchDragAnywhere = io.MouseSource == ImGuiMouseSource_TouchScreen
			&& io.MouseDown[mouse_button]
			&& window->Rect().Contains(io.MousePos)
			&& ImGui::IsMouseDragging(mouse_button, io.MouseDragThreshold);
	if (touchDragAnywhere)
	{
		// Android finger scrolling often starts over a live Selectable row. Treat
		// that as a scroll drag so settings rows do not eat the swipe as a tap.
		const ImVec2& delta = io.MouseDelta;
		if (delta != ImVec2())
		{
			window->DragScrolling = true;
			window->ScrollSpeed = delta;
		}
		return;
	}
#endif
    bool hovered = false;
    bool held = false;
    ImGuiButtonFlags button_flags = (mouse_button == ImGuiMouseButton_Left) ? ImGuiButtonFlags_MouseButtonLeft
    		: (mouse_button == ImGuiMouseButton_Right) ? ImGuiButtonFlags_MouseButtonRight : ImGuiButtonFlags_MouseButtonMiddle;
    // If nothing hovered so far in the frame (not same as IsAnyItemHovered()!) or item is disabled
	if (g.HoveredId == 0 || g.HoveredIdIsDisabled)
    {
    	bool hoveredAllowOverlap = g.HoveredIdAllowOverlap;
    	g.HoveredIdAllowOverlap = true;
    	ImGuiID overlayId = window->GetID("##scrolldraggingoverlay");
    	ImGui::ButtonBehavior(window->Rect(), overlayId, &hovered, &held, button_flags);
    	ImGui::KeepAliveID(overlayId);
    	g.HoveredIdAllowOverlap = hoveredAllowOverlap;
    }
    const ImVec2& delta = ImGui::GetIO().MouseDelta;
    if (held && delta != ImVec2())
    {
    	window->DragScrolling = true;
    	window->ScrollSpeed = delta;
    }
}

// Helper to display a little (?) mark which shows a tooltip when hovered.
void ShowHelpMarker(const char* desc)
{
    ImGui::TextDisabled("%s", T("(?)"));
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

template<bool PerGameOption>
bool OptionCheckbox(const char *name, config::Option<bool, PerGameOption>& option, const char *help)
{
	bool pressed;
	{
		DisabledScope scope(option.isReadOnly());

		bool b = option;
		pressed = ImGui::Checkbox(name, &b);
		if (pressed)
			option.set(b);
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return pressed;
}
template bool OptionCheckbox(const char *name, config::Option<bool, true>& option, const char *help);
template bool OptionCheckbox(const char *name, config::Option<bool, false>& option, const char *help);

template<bool PerGameOption>
bool OptionSlider(const char *name, config::Option<int, PerGameOption>& option, int min, int max, const char *help, const char *format)
{
	bool valueChanged;
	{
		DisabledScope scope(option.isReadOnly());

		int v = option;
		valueChanged = ImGui::SliderInt(name, &v, min, max, format);
		if (valueChanged)
			option.set(v);
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return valueChanged;
}
template bool OptionSlider(const char *name, config::Option<int, true>& option, int min, int max, const char *help, const char *format);
template bool OptionSlider(const char *name, config::Option<int, false>& option, int min, int max, const char *help, const char *format);

bool OptionArrowButtons(const char *name, config::Option<int>& option, int min, int max, const char *help, const char *format)
{
	const float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
	const std::string id = "##" + std::string(name);
	{
		ImguiStyleVar _(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)); // Left
		ImguiStyleColor _1(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
		const float width = ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f;
		ImguiStyleVar _2(ImGuiStyleVar_DisabledAlpha, 1.0f);
		ImGui::BeginDisabled();
		std::string value = strprintf(format, (int)option);
		ImGui::ButtonEx((value + id).c_str(), ImVec2(width, 0));
		ImGui::EndDisabled();
	}

	ImGui::SameLine(0.0f, innerSpacing);
	ImGui::PushButtonRepeat(true);
	bool valueChanged = false;
	{
		DisabledScope scope(option.isReadOnly());

		if (ImGui::ArrowButton((id + "left").c_str(), ImGuiDir_Left)) { option.set(std::max(min, option - 1)); valueChanged = true; }
		ImGui::SameLine(0.0f, innerSpacing);
		if (ImGui::ArrowButton((id + "right").c_str(), ImGuiDir_Right)) { option.set(std::min(max, option + 1)); valueChanged = true; }
	}
	ImGui::PopButtonRepeat();
	ImGui::SameLine(0.0f, innerSpacing);
	ImGui::Text("%s", name);
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return valueChanged;
}

template<typename T>
bool OptionRadioButton(const char *name, config::Option<T>& option, T value, const char *help)
{
	bool pressed;
	{
		DisabledScope scope(option.isReadOnly());

		int v = (int)option;
		pressed = ImGui::RadioButton(name, &v, (int)value);
		if (pressed)
			option.set((T)v);
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return pressed;
}
template bool OptionRadioButton<bool>(const char *name, config::Option<bool>& option, bool value, const char *help);
template bool OptionRadioButton<int>(const char *name, config::Option<int>& option, int value, const char *help);

template<bool PerGameOption>
void OptionComboBox(const char *name, config::Option<int, PerGameOption>& option, const char *values[], int count,
			const char *help)
{
	{
		DisabledScope scope(option.isReadOnly());

		const char *value = option >= 0 && option < count ? values[option] : "?";
		if (ImGui::BeginCombo(name, value, ImGuiComboFlags_None))
		{
			for (int i = 0; i < count; i++)
			{
				bool is_selected = option == i;
				if (ImGui::Selectable(values[i], &is_selected))
					option = i;
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
}

// Explicit template instantiations
template void OptionComboBox<true>(const char *name, config::Option<int, true>& option, const char *values[], int count, const char *help);
template void OptionComboBox<false>(const char *name, config::Option<int, false>& option, const char *values[], int count, const char *help);

bool SettingsRow(const char* label, const char* currentValue, const char* helpText)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);

	// Create selectable that spans both columns (full row is clickable)
	std::string selectableId = std::string("##row_") + label;
	bool clicked = ImGui::Selectable(selectableId.c_str(), false,
	                                 ImGuiSelectableFlags_SpanAllColumns |
	                                 ImGuiSelectableFlags_AllowOverlap);

	// Calculate position for label (left column)
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	float labelX = cursorPos.x + ImGui::GetStyle().CellPadding.x;
	float labelY = cursorPos.y + ImGui::GetStyle().FramePadding.y;

	// Draw label
	ImGui::SetCursorScreenPos(ImVec2(labelX, labelY));
	ImGui::TextUnformatted(label);

	// Calculate position for value (right column)
	float valueX = cursorPos.x + ImGui::GetColumnWidth(0) + ImGui::GetStyle().CellPadding.x * 2.0f;
	float valueY = cursorPos.y + ImGui::GetStyle().FramePadding.y;

	// Draw current value
	ImGui::SetCursorScreenPos(ImVec2(valueX, valueY));
	ImGui::TextUnformatted(currentValue);

	// Help marker
	if (helpText != nullptr)
	{
		float valueWidth = ImGui::CalcTextSize(currentValue).x;
		ImGui::SetCursorScreenPos(ImVec2(valueX + valueWidth + ImGui::GetStyle().ItemSpacing.x, valueY));
		ShowHelpMarker(helpText);
	}

	return clicked;
}

int SelectionPopup(const char* popupId, const char* title, const char* options[], int optionCount, int currentSelection)
{
	int selectedIndex = -1;

	if (!ImGui::IsPopupOpen(popupId))
		return -1;

	// Center the popup
	centerNextWindow();

	// Set popup size (auto width, constrained height)
	float maxWidth = ImGui::GetIO().DisplaySize.x * 0.8f;
	float maxHeight = ImGui::GetIO().DisplaySize.y * 0.6f;
	ImGui::SetNextWindowSize(ImVec2(maxWidth, 0), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(maxWidth, maxHeight));

	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
								   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	if (ImGui::BeginPopupModal(popupId, nullptr, windowFlags))
	{
		// Title
		if (title != nullptr)
		{
			ImGui::TextUnformatted(title);
			ImGui::Separator();
			ImGui::Spacing();
		}

		// Options list
		for (int i = 0; i < optionCount; i++)
		{
			bool isSelected = (i == currentSelection);
			if (ImGui::Selectable(options[i], isSelected, ImGuiSelectableFlags_DontClosePopups))
			{
				selectedIndex = i;
				ImGui::CloseCurrentPopup();
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}

		// Close on Escape or click outside
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)
			|| (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
				&& !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)))
		{
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	return selectedIndex;
}

void fullScreenWindow(bool modal)
{
	if (!modal)
	{
		ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
		ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);

		if (insetLeft > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(insetLeft, ImGui::GetIO().DisplaySize.y));
			ImGui::Begin("##insetLeft", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
		if (insetRight > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - insetRight, 0));
			ImGui::SetNextWindowSize(ImVec2(insetRight, ImGui::GetIO().DisplaySize.y));
			ImGui::Begin("##insetRight", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
		if (insetTop > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, insetTop));
			ImGui::Begin("##insetTop", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
		if (insetBottom > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y - insetBottom));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, insetBottom));
			ImGui::Begin("##insetBottom", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
	}
	// Position the main window below the menu bar only when it is actually visible.
	float menuBarHeight = GuiMenu::mainMenuBarHeight();
	ImGui::SetNextWindowPos(ImVec2(insetLeft, insetTop + menuBarHeight));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - insetLeft - insetRight, ImGui::GetIO().DisplaySize.y - insetTop - insetBottom - menuBarHeight));
}

static void computeScrollSpeed(float &v)
{
	constexpr float friction = 3.f;
	if (std::abs(v) > friction)
	{
		float sign = (v > 0.f) - (v < 0.f);
		v -= friction * sign;
	}
	else
	{
		v = 0.f;
	}
}

void windowDragScroll()
{
	ImGuiWindow *window = ImGui::GetCurrentWindow();
	if (window->DragScrolling)
	{
		if (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left])
		{
			computeScrollSpeed(window->ScrollSpeed.x);
			computeScrollSpeed(window->ScrollSpeed.y);
			if (window->ScrollSpeed == ImVec2())
			{
				window->DragScrolling = false;
				// FIXME we should really move the mouse off-screen after a touch up and this wouldn't be necessary
				// the only problem is tool tips
				gui_set_mouse_position(-1, -1, true);
			}
		}
		else
		{
			ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
			if (delta != ImVec2())
				ImGui::ResetMouseDragDelta();
			window->ScrollSpeed = delta;
		}
		if (window->DragScrolling)
		{
			ImGui::SetScrollX(window, window->Scroll.x - window->ScrollSpeed.x);
			ImGui::SetScrollY(window, window->Scroll.y - window->ScrollSpeed.y);
		}
	}
}

static void setUV(float ar, ImVec2& uv0, ImVec2& uv1)
{
	uv0 = { 0.f, 0.f };
	uv1 = { 1.f, 1.f };
	if (ar > 1)
	{
		uv0.y = -(ar - 1) / 2;
		uv1.y = 1 + (ar - 1) / 2;
	}
	else if (ar != 0)
	{
		ar = 1 / ar;
		uv0.x = -(ar - 1) / 2;
		uv1.x = 1 + (ar - 1) / 2;
	}
}

void ImguiTexture::draw(const ImVec2& size, const ImVec4& tint_col, const ImVec4& border_col)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		ImGui::Dummy(size);
	else
	{
		const float ar = imguiDriver->getAspectRatio(id);
		ImVec2 drawSize(size);
		if (size.x == 0.f)
			drawSize.x = size.y * ar;
		else if (size.y == 0.f)
			drawSize.y = size.x / ar;
		ImVec2 uv0, uv1;
		setUV(ar / drawSize.x * drawSize.y, uv0, uv1);
		ImGui::Image(id, drawSize, uv0, uv1, tint_col, border_col);
	}
}

void ImguiTexture::draw(ImDrawList *drawList, const ImVec2& pos, const ImVec2& size, float alpha)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		return;
	const float ar = imguiDriver->getAspectRatio(id);
	ImVec2 uv0, uv1;
	setUV(ar / size.x * size.y, uv0, uv1);
	u32 col = alphaOverride(0xffffff, alpha);
	drawList->AddImage(id, pos, pos + size, uv0, uv1, col);
}

void ImguiTexture::draw(ImDrawList *drawList, const ImVec2& pos, const ImVec2& size,
		const ImVec2& uv0, const ImVec2& uv1, const ImVec4& color)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		return;
	u32 col = ImGui::ColorConvertFloat4ToU32(color);
	drawList->AddImage(id, pos, pos + size, uv0, uv1, col);
}

bool ImguiTexture::button(const char* str_id, const ImVec2& image_size, const std::string& title,
		const ImVec4& bg_col, const ImVec4& tint_col, float fallbackTitleSize)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
	{
		if (fallbackTitleSize <= 0.0f || title.empty())
			return ImGui::Button(title.c_str(), image_size);

		ImGui::PushFont(nullptr, fallbackTitleSize);
		const bool pressed = ImGui::Button(str_id, image_size);
		const ImVec2 min = ImGui::GetItemRectMin() + ImGui::GetStyle().FramePadding;
		const ImVec2 max = ImGui::GetItemRectMax() - ImGui::GetStyle().FramePadding;
		const float wrapWidth = std::max(1.0f, max.x - min.x);
		const ImVec4 clipRect(min.x, min.y, max.x, max.y);
		ImFont *font = ImGui::GetFont();
		const float fontSize = ImGui::GetFontSize();
		const char *text = title.c_str();
		const char *textEnd = text + title.size();
		std::vector<std::pair<const char *, const char *>> lines;
		for (const char *line = text; line < textEnd; )
		{
			const char *lineEnd = font->CalcWordWrapPosition(fontSize, line, textEnd, wrapWidth);
			if (lineEnd == line)
				lineEnd = textEnd;
			lines.emplace_back(line, lineEnd);
			line = ImTextCalcWordWrapNextLineStart(lineEnd, textEnd);
		}

		const float lineHeight = ImGui::GetTextLineHeight();
		float y = min.y + std::max(0.0f, (max.y - min.y - lineHeight * lines.size()) * 0.5f);
		for (const auto& line : lines)
		{
			const float lineWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, line.first, line.second).x;
			const ImVec2 textPos(min.x + std::max(0.0f, (max.x - min.x - lineWidth) * 0.5f), y);
			ImGui::GetWindowDrawList()->AddText(font, fontSize, textPos, ImGui::GetColorU32(ImGuiCol_Text),
					line.first, line.second, 0.0f, &clipRect);
			y += lineHeight;
			if (y > max.y)
				break;
		}
		ImGui::PopFont();
		return pressed;
	}
	else
	{
		const float ar = imguiDriver->getAspectRatio(id);
		const ImVec2 size = image_size - ImGui::GetStyle().FramePadding * 2;
		ImVec2 uv0, uv1;
		setUV(ar / size.x * size.y, uv0, uv1);
		return ImGui::ImageButton(str_id, id, size, uv0, uv1, bg_col, tint_col);
	}
}

static u8 *loadImage(const std::string& path, int& width, int& height)
{
	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(path, "rb"));
	if (file == nullptr)
		return nullptr;

	int channels;
	stbi_set_flip_vertically_on_load_thread(0);
	const s64 fileSize = file->size();
	if (fileSize <= 0)
		return nullptr;
	std::vector<u8> data(fileSize);
	if (file->read(data.data(), 1, data.size()) != data.size())
		return nullptr;
	return stbi_load_from_memory(data.data(), data.size(), &width, &height, &channels, STBI_rgb_alpha);
}

int ImguiFileTexture::textureLoadCount;

ImTextureID ImguiFileTexture::getId()
{
	if (path.empty())
		return {};
	ImTextureID id = imguiDriver->getTexture(path);
	if (id == ImTextureID())
	{
		constexpr int MaxTextureLoadsPerFrame = 5;
		if (textureLoadCount < MaxTextureLoadsPerFrame)
		{
			textureLoadCount++;
			int width, height;
			u8 *imgData = loadImage(path, width, height);
			if (imgData != nullptr)
			{
				try {
					id = imguiDriver->updateTextureAndAspectRatio(path, imgData, width, height, nearestSampling);
				} catch (...) {
					// vulkan can throw during resizing
				}
				free(imgData);
			}
		}
	}
	return id;
}

std::future<ImguiStateTexture::LoadedPic> ImguiStateTexture::asyncLoad;

bool ImguiStateTexture::exists()
{
	std::string path = hostfs::getSavestatePath(slot, false);
	return hostfs::storage().exists(path);
}

ImTextureID ImguiStateTexture::getId()
{
	const int stateSlot = slot;
	std::string path = hostfs::getSavestatePath(stateSlot, false);
	ImTextureID texid = imguiDriver->getTexture(path);
	if (texid != ImTextureID())
		return texid;
	if (asyncLoad.valid())
	{
		if (asyncLoad.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
			return {};
		LoadedPic loadedPic = asyncLoad.get();
		if (loadedPic.data != nullptr)
		{
			try {
				texid = imguiDriver->updateTextureAndAspectRatio(path, loadedPic.data, loadedPic.width, loadedPic.height, nearestSampling);
			} catch (...) {
				// vulkan can throw during resizing
			}
			free(loadedPic.data);
		}
		return texid;
	}
	asyncLoad = std::async(std::launch::async, [stateSlot]() {
		LoadedPic loadedPic{};
		// load savestate info
		std::vector<u8> pngData;
		dc_getStateScreenshot(stateSlot, pngData);
		if (pngData.empty())
			return loadedPic;

		int channels;
		stbi_set_flip_vertically_on_load_thread(0);
		loadedPic.data = stbi_load_from_memory(&pngData[0], pngData.size(), &loadedPic.width, &loadedPic.height, &channels, STBI_rgb_alpha);

		return loadedPic;
	});
	return {};
}

void ImguiStateTexture::invalidate()
{
	if (imguiDriver)
	{
		std::string path = hostfs::getSavestatePath(slot, false);
		imguiDriver->deleteTexture(path);
	}
}

std::array<ImguiVmuTexture, 8> ImguiVmuTexture::Vmus { 0, 1, 2, 3, 4, 5, 6, 7 };
constexpr float VMU_WIDTH = 96.f;
constexpr float VMU_HEIGHT = 64.f;
constexpr float VMU_PADDING = 8.f;

ImTextureID ImguiVmuTexture::getId()
{
	if (!vmu_lcd_status[index])
		return {};
	if (idPath.empty())
		idPath = ":vmu:" + std::to_string(index);
	ImTextureID texid = imguiDriver->getTexture(idPath);
	if (texid == ImTextureID() || vmuLastChanged != ::vmuLastChanged[index])
	{
		try {
			texid = imguiDriver->updateTexture(idPath, (const u8 *)vmu_lcd_data[index], 48, 32, true);
			vmuLastChanged = ::vmuLastChanged[index];
		} catch (...) {
		}
	}
	return texid;
}

void ImguiVmuTexture::displayVmus(const ImVec2& pos)
{
	const ScaledVec2 size(VMU_WIDTH, VMU_HEIGHT);
	const float padding = uiScaled(VMU_PADDING);
	assert((ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_ChildWindow) == 0);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 cpos(pos + ScaledVec2(2.f, 0));	// 96 pixels wide + 2 * 2 -> 100
	for (int i = 0; i < 8; i++)
	{
		if (!vmu_lcd_status[i])
			continue;

		ImTextureID texid = Vmus[i].getId();
		if (texid == ImTextureID())
			continue;
		ImVec2 pos_b = cpos + size;
		dl->AddImage(texid, cpos, pos_b, ImVec2(0, 1), ImVec2(1, 0), 0x80ffffff);
		cpos.y += size.y + padding;
	}
}

void Toast::show(const std::string& title, const std::string& message, u32 durationMs)
{
	const u64 now = getTimeMs();
	std::lock_guard<std::mutex> _{mutex};
	// no start anim if still visible
	if (now > endTime + END_ANIM_TIME)
		startTime = getTimeMs();
	endTime = now + durationMs;
	this->title = title;
	this->message = message;
}

bool Toast::draw()
{
	const u64 now = getTimeMs();
	std::lock_guard<std::mutex> _{mutex};
	if (now > endTime + END_ANIM_TIME) {
		title.clear();
		message.clear();
	}
	if (title.empty() && message.empty())
		return false;
	float alpha = 1.f;
	if (now > endTime)
		// Fade out
		alpha = (std::cos((now - endTime) / (float)END_ANIM_TIME * (float)M_PI) + 1.f) / 2.f;

	const ImVec2 displaySize(ImGui::GetIO().DisplaySize);
	const float maxW = std::min(uiScaled(640.f), displaySize.x);
	ImFont *regularFont = ImGui::GetFont();
	const ImVec2 titleSize = title.empty() ? ImVec2()
			: ImGui::GetFont()->CalcTextSizeA(uiLargeFontSize(), FLT_MAX, maxW, &title.front(), &title.back() + 1);
	const float regularFontSize = ImGui::GetStyle().FontSizeBase;
	const ImVec2 msgSize = message.empty() ? ImVec2()
			: regularFont->CalcTextSizeA(regularFontSize, FLT_MAX, maxW, &message.front(), &message.back() + 1);
	const ScaledVec2 padding(5.f, 4.f);
	const ScaledVec2 spacing(0.f, 2.f);
	ImVec2 totalSize(std::max(titleSize.x, msgSize.x), titleSize.y + msgSize.y);
	totalSize += padding * 2.f + spacing * (float)(!title.empty() && !message.empty());

	ImVec2 pos(insetLeft, displaySize.y - totalSize.y);
	if (now - startTime < START_ANIM_TIME)
		// Slide up
		pos.y += totalSize.y * (std::cos((now - startTime) / (float)START_ANIM_TIME * (float)M_PI) + 1.f) / 2.f;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImU32 bg_col = alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), alpha / 2.f);
	dl->AddRectFilled(pos, pos + totalSize, bg_col, 0.f);
	const ImU32 col = alphaOverride(ImGui::GetColorU32(ImGuiCol_Border), alpha);
	dl->AddRect(pos, pos + totalSize, col, 0.f);

	pos += padding;
	if (!title.empty())
	{
		const ImU32 col = alphaOverride(ImGui::GetColorU32(ImGuiCol_Text), alpha);
		dl->AddText(nullptr, uiLargeFontSize(), pos, col, &title.front(), &title.back() + 1, maxW);
		pos.y += spacing.y + titleSize.y;
	}
	if (!message.empty())
	{
		const ImU32 col = alphaOverride(0xFF00FFFF, alpha);	// yellow
		dl->AddText(regularFont, regularFontSize, pos, col, &message.front(), &message.back() + 1, maxW);
	}

	return true;
}

std::string middleEllipsis(const std::string& s, float width)
{
	float tw = ImGui::CalcTextSize(s.c_str()).x;
	if (tw <= width)
		return s;
	char buf[5];
	ImTextCharToUtf8(buf, ImGui::GetFont()->EllipsisChar);
	std::string ellipsis = buf;

	int l = s.length() / 2;
	int d = l;

	while (true)
	{
		std::string ss = s.substr(0, l / 2) + ellipsis + s.substr(s.length() - l / 2 - (l & 1));
		tw = ImGui::CalcTextSize(ss.c_str()).x;
		if (tw == width)
			return ss;
		d /= 2;
		if (d == 0)
			return ss;
		if (tw > width)
			l -= d;
		else
			l += d;
	}
}

bool beginFrame(const char *label, const ImVec2& size_arg, ImVec2 *out_size)
{
	using namespace ImGui;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;
    const ImGuiStyle& style = g.Style;
	const ImVec2 label_size = CalcTextSize(label, nullptr, true);
    ImVec2 size = ImTrunc(CalcItemSize(size_arg, CalcItemWidth(), GetTextLineHeightWithSpacing() * 7.25f + style.FramePadding.y * 2.0f));
    ImVec2 frame_size = ImVec2(size.x, ImMax(size.y, label_size.y));
    ImRect frame_bb(window->DC.CursorPos, window->DC.CursorPos + frame_size);
    ImRect bb(frame_bb.Min, frame_bb.Max + ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0.0f));
    window->DC.CursorMaxPos = ImMax(window->DC.CursorMaxPos, bb.Max);

    BeginGroup();
    if (label_size.x > 0.0f)
    {
        ImVec2 label_pos = ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + style.FramePadding.y);
        RenderText(label_pos, label);
        window->DC.CursorMaxPos = ImMax(window->DC.CursorMaxPos, label_pos + label_size);
    }

    const ImU32 bg_col = GetColorU32(ImGuiCol_FrameBg);
    window->DrawList->AddRectFilled(frame_bb.Min, frame_bb.Max, bg_col, g.Style.FrameRounding, 0);
    window->DC.CursorPos += style.FramePadding;
    PushClipRect(frame_bb.Min + style.FramePadding, frame_bb.Max - style.FramePadding, false);
    if (out_size != nullptr)
    	*out_size = frame_size - style.FramePadding * 2.f;
    BeginGroup();

    return true;
}

void endFrame()
{
	using namespace ImGui;
	EndGroup();
	PopClipRect();
	EndGroup();
}

#ifdef __SWITCH__

static constexpr unsigned Flags_Multiline = 1 << 31;

bool switchEditText(char *value, size_t capacity, ImGuiInputTextFlags flags, bool multiline);

static int switchInputTextCallback(ImGuiInputTextCallbackData *data)
{
	if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && (data->Flags & ImGuiInputTextFlags_ReadOnly) == 0)
	{
		data->Buf[data->BufTextLen] = '\0';
		if (switchEditText(data->Buf, data->BufSize, data->Flags, (data->Flags & Flags_Multiline) != 0))
		{
			data->BufDirty = true;
			data->BufTextLen = strlen(data->Buf);
			ImGui::ClearActiveID();
			return 1;
		}
		ImGui::ClearActiveID();
	}
	return 0;
}
#endif

bool InputText(const char *label, std::string *str, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
#ifdef __SWITCH__
	if ((flags & ImGuiInputTextFlags_ReadOnly) == 0)
	{
		// TODO This doesn't handle growing the string capacity dynamically
		str->reserve(512);
		return ImGui::InputText(label, str, flags | ImGuiInputTextFlags_CallbackAlways, switchInputTextCallback);
	}
#endif
	return ImGui::InputText(label, str, flags, callback, user_data);
}

bool InputText(const char *label, char *str, size_t size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
#ifdef __SWITCH__
	if ((flags & ImGuiInputTextFlags_ReadOnly) == 0)
		return ImGui::InputText(label, str, size, flags | ImGuiInputTextFlags_CallbackAlways, switchInputTextCallback);
#endif
	return ImGui::InputText(label, str, size, flags, callback, user_data);
}

bool InputTextMultiline(const char* label, char* buf, size_t buf_size, const ImVec2& size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
#ifdef __SWITCH__
	if ((flags & ImGuiInputTextFlags_ReadOnly) == 0)
		return ImGui::InputTextMultiline(label, buf, buf_size, size, flags | ImGuiInputTextFlags_CallbackAlways | Flags_Multiline, switchInputTextCallback);
#endif
	return ImGui::InputTextMultiline(label, buf, buf_size, size, flags, callback, user_data);
}

// ============================================================================
// Phase 0 Widget Infrastructure Components
// ============================================================================

void SectionDivider(const char* text)
{
	ImGuiStyle& style = ImGui::GetStyle();

	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Separator, style.Colors[ImGuiCol_Border]);
	ImGui::Separator();
	ImGui::PopStyleColor();

	if (text != nullptr && text[0] != '\0')
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_Text]);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
}

void SectionHeaderWithIcon(const char* icon, const char* text)
{
	ImGui::Spacing();

	// Icon + text with highlighted color
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
	ImGui::PushFont(settingsIconFont);
	ImGui::Text("%s", icon);
	ImGui::PopFont();
	ImGui::SameLine();
	ImGui::Text("%s", text);
	ImGui::PopStyleColor();

	// Separator line
	ImGui::Separator();

	ImGui::Spacing();
}

void SettingIcon(const char* icon, const ImVec2& size)
{
	ImGuiStyle& style = ImGui::GetStyle();

	ImVec2 iconSize = size;
	if (iconSize.x <= 0 || iconSize.y <= 0)
		iconSize = ImVec2(settings.display.uiScale * 16, settings.display.uiScale * 16);

	const ImVec2 iconPos = ImGui::GetCursorScreenPos();
	ImFont* font = settingsIconFont;

	// FontAwesome glyphs do not always fit their box at every UI scale, so measure
	// and shrink only when needed before drawing the icon centered in the row slot.
	float fontSize = std::min(iconSize.x, iconSize.y);
	ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, icon);
	if (textSize.x > iconSize.x && textSize.x > 0.0f)
	{
		fontSize *= iconSize.x / textSize.x;
		textSize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, icon);
	}

	const ImVec2 textPos(
		iconPos.x + (iconSize.x - textSize.x) * 0.5f,
		iconPos.y + (iconSize.y - textSize.y) * 0.5f
	);
	ImGui::GetWindowDrawList()->AddText(
		font,
		fontSize,
		textPos,
		ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_Text]),
		icon
	);
	ImGui::Dummy(iconSize);
}

void BeginSettingsRow(const SettingsRowParams& params)
{
	ImGui::BeginChild(params.label, ImVec2(0, params.minHeight > 0 ? params.minHeight : settings.display.uiScale * 48),
		ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);

	// Draw icon if provided
	if (params.icon != nullptr)
	{
		ImGui::PushFont(settingsIconFont);
		ImGui::Text("%s", params.icon);
		ImGui::PopFont();
		ImGui::SameLine(0, settings.display.uiScale * 12);
	}

	// Draw label (bold)
	ImGui::TextUnformatted(params.label);

	// Reserve space for control on right
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - settings.display.uiScale * 100);
}

void EndSettingsRow()
{
	ImGui::EndChild();
	ImGui::Spacing();
}

bool ToggleSwitch(const char* label, bool* value, const char* helpText)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiID id = window->GetID(label);

	const float height = settings.display.uiScale * 24;
	const float width = settings.display.uiScale * 48;
	const float radius = height * 0.5f;
	const ImVec2 pos = window->DC.CursorPos;
	const ImVec2 size(width, height);

	const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
	ImGui::ItemSize(size, ImGui::GetStyle().FramePadding.y);
	if (!ImGui::ItemAdd(bb, id))
		return false;

	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

	if (pressed)
	{
		*value = !*value;
		return true;
	}

	// Animation
	float anim = 0;
	if (g.LastActiveId == id)
	{
		float t = ImMin((float)(g.Time - g.LastActiveIdTimer) / 0.15f, 1.0f);
		anim = *value ? t : (1.0f - t);
	}
	else
	{
		anim = *value ? 1.0f : 0.0f;
	}

	// Render
	ImU32 col_bg;
	if (*value)
		col_bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
	else
		col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	draw_list->AddRectFilled(bb.Min, bb.Max, col_bg, radius);

	// Knob - use theme-aware color
	ImVec2 knob_pos;
	knob_pos.x = ImLerp(bb.Min.x + radius, bb.Max.x - radius, anim);
	knob_pos.y = bb.Min.y + radius;

	// Detect light vs dark theme
	ImGuiStyle& guiStyle = ImGui::GetStyle();
	float bgLuminance = (guiStyle.Colors[ImGuiCol_WindowBg].x
	                   + guiStyle.Colors[ImGuiCol_WindowBg].y
	                   + guiStyle.Colors[ImGuiCol_WindowBg].z) / 3.0f;
	bool isLightTheme = bgLuminance > 0.5f;

	ImU32 knobColor = isLightTheme
		? ImGui::GetColorU32(ImGuiCol_Text)  // Dark text color for light themes
		: IM_COL32(255, 255, 255, 255);      // White for dark themes
	draw_list->AddCircleFilled(knob_pos, radius - 1.0f, knobColor);

	return pressed;
}

template<bool PerGameOption>
bool SettingsOption(const char* label, config::Option<bool, PerGameOption>& option,
                   const char* description, const char* icon, const char* helpText)
{
	BeginSettingsRow({label, description, icon, nullptr, settings.display.uiScale * 48});

	bool value = option;
	bool changed = ToggleSwitch(("##" + std::string(label)).c_str(), &value);
	if (changed)
		option = value;

	EndSettingsRow();

	if (helpText != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(helpText);
	}

	return changed;
}

// Explicit template instantiations for SettingsOption
template bool SettingsOption<true>(const char* label, config::Option<bool, true>& option,
                                   const char* description, const char* icon, const char* helpText);
template bool SettingsOption<false>(const char* label, config::Option<bool, false>& option,
                                    const char* description, const char* icon, const char* helpText);

namespace SettingsUI {

namespace Detail {

// DuckStation-style layout constants
namespace Layout {
    constexpr float SMALL_POPUP_PADDING = 20.0f;
    constexpr float MENU_BUTTON_PADDING = 8.0f;
    constexpr float MENU_BUTTON_HEIGHT = 28.0f;
    constexpr float MENU_BUTTON_SPACING = 8.0f;
    constexpr float WIDGET_FRAME_ROUNDING = 4.0f;
    constexpr float POPUP_WIDTH = 600.0f;
    constexpr float POPUP_MIN_WIDTH = 300.0f;
    constexpr float POPUP_ROUNDING = 18.0f;
    constexpr float POPUP_VALUE_RIGHT_PADDING = 28.0f;
}

// Color helper functions (DuckStation-style)
static inline ImVec4 ModAlpha(const ImVec4& v, float a)
{
    return ImVec4(v.x, v.y, v.z, a);
}

static inline u32 ModAlpha(u32 col32, float a)
{
    return (col32 & ~IM_COL32_A_MASK) | (static_cast<u32>(a * 255.0f) << IM_COL32_A_SHIFT);
}

static inline ImVec4 DarkerColor(const ImVec4& v, float f = 0.8f)
{
    return ImVec4(std::max(v.x, 1.0f / 255.0f) * f,
                std::max(v.y, 1.0f / 255.0f) * f,
                std::max(v.z, 1.0f / 255.0f) * f, v.w);
}

static inline float ColorLuminance(const ImVec4& v)
{
    return v.x * 0.299f + v.y * 0.587f + v.z * 0.114f;
}

static inline ImVec4 BlendColor(const ImVec4& color, const ImVec4& target, float amount)
{
    return ImVec4(
        color.x + (target.x - color.x) * amount,
        color.y + (target.y - color.y) * amount,
        color.z + (target.z - color.z) * amount,
        color.w
    );
}

static ImVec4 ReadablePopupTextColor(const ImVec4& bg)
{
    return ColorLuminance(bg) > 0.50f
        ? ImVec4(0.06f, 0.06f, 0.07f, 1.00f)
        : ImVec4(0.95f, 0.95f, 0.97f, 1.00f);
}

static ImVec4 ReadablePopupDisabledTextColor(const ImVec4& bg)
{
    return ColorLuminance(bg) > 0.50f
        ? ImVec4(0.32f, 0.32f, 0.36f, 1.00f)
        : ImVec4(0.72f, 0.72f, 0.76f, 1.00f);
}

static ImVec4 ReadablePopupFillColor(ImVec4 fill, const ImVec4& text)
{
    const bool lightText = ColorLuminance(text) > 0.50f;
    const ImVec4 target = lightText ? ImVec4(0.0f, 0.0f, 0.0f, fill.w) : ImVec4(1.0f, 1.0f, 1.0f, fill.w);
    const float limit = lightText ? 0.42f : 0.58f;
    for (int i = 0; i < 4; i++)
    {
        const float luminance = ColorLuminance(fill);
        if ((lightText && luminance <= limit) || (!lightText && luminance >= limit))
            break;
        fill = BlendColor(fill, target, 0.25f);
    }
    return fill;
}

// Helper: Apply DuckStation-style popup styling
struct PopupStyleScope {
    PopupStyleScope(float padding = Layout::SMALL_POPUP_PADDING, float rounding = Layout::POPUP_ROUNDING) {
        // Popup colors are derived from the current surface so theme switches
        // cannot leave light-theme text on a dark dialog, or the reverse.
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4 popupBg = style.Colors[ImGuiCol_PopupBg];
        ImVec4 popupText = ReadablePopupTextColor(popupBg);
        ImVec4 popupTextDisabled = ReadablePopupDisabledTextColor(popupBg);

        // DuckStation-style popup background with full opacity
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ModAlpha(popupBg, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, popupText);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, popupTextDisabled);
        ImGui::PushStyleColor(ImGuiCol_Header, ReadablePopupFillColor(style.Colors[ImGuiCol_Header], popupText));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ReadablePopupFillColor(style.Colors[ImGuiCol_HeaderHovered], popupText));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ReadablePopupFillColor(style.Colors[ImGuiCol_HeaderActive], popupText));
        // Button active state (darker for pressed state)
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ModAlpha(DarkerColor(popupBg, 1.8f), 1.0f));
        // Button hovered state (medium dark)
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ModAlpha(DarkerColor(popupBg, 1.3f), 1.0f));
        // Frame background for input widgets
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ModAlpha(DarkerColor(popupBg, 0.8f), 0.5f));

        // DuckStation-style window padding and rounding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(uiScaled(padding), uiScaled(padding)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, uiScaled(rounding));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, uiScaled(rounding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(Layout::WIDGET_FRAME_ROUNDING));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(Layout::MENU_BUTTON_SPACING), uiScaled(Layout::MENU_BUTTON_SPACING)));
    }

    ~PopupStyleScope() {
        ImGui::PopStyleVar(6);
        ImGui::PopStyleColor(9);
    }
};

// Helper: Begin menu button list (DuckStation-style)
struct BeginMenuButtons {
    BeginMenuButtons() {
        // Apply proper spacing for menu buttons
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(uiScaled(Layout::MENU_BUTTON_PADDING), uiScaled(Layout::MENU_BUTTON_PADDING)));
        // Focus reset for keyboard navigation
        ImGui::SetNextItemWidth(-1.0f);
    }
    ~BeginMenuButtons() {
        ImGui::PopStyleVar(2);
    }
};

// Helper: End menu button list (DuckStation-style)
struct EndMenuButtons {
    EndMenuButtons() = default;
};

// Internal helper to render options popup
bool RenderOptionsPopup(const PopupOptionsConfig& cfg)
{
    // Icon and row label
    SettingIcon(cfg.icon, ImVec2(uiScaled(cfg.iconSize), uiScaled(cfg.iconSize)));
    ImGui::SameLine(0, uiScaled(cfg.iconSpacing));
    const char* rowLabel = cfg.label ? cfg.label : "";
    ImGui::PushFont(largeFont);
    ImGui::TextUnformatted(rowLabel);
    ImGui::PopFont();

    // Get current display string
    const char* currentStr = "";
    if (cfg.valueToString) {
        currentStr = cfg.valueToString(*cfg.currentValue);
    } else {
        int idx = (*cfg.currentValue >= 0 && *cfg.currentValue < cfg.optionCount)
                  ? *cfg.currentValue : 0;
        currentStr = cfg.options[idx];
    }
    if (currentStr == nullptr)
        currentStr = "";

    std::string displayValue = currentStr;
    if (cfg.disabled && cfg.disabledPrefix && cfg.disabledPrefix[0] != '\0')
        displayValue.insert(0, cfg.disabledPrefix);

    const float valueWidth = uiScaled(cfg.valueWidth);
    const float valueRightPadding = uiScaled(cfg.valueRightPadding);
    const float rowRightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float slotStartX = rowRightX - valueWidth - valueRightPadding;
    const float toggleCenterX = rowRightX - uiScaled(28.0f) - uiScaled(50.0f) * 0.5f;
    const float minRightMargin = uiScaled(16.0f);
    ImGui::SameLine(slotStartX);
    if (cfg.valueVerticalOffset != 0.0f) {
        const ImVec2 pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(pos.x, pos.y + cfg.valueVerticalOffset));
    }

    auto centeredValueX = [&](float textWidth) {
        const float maxX = rowRightX - minRightMargin - textWidth;
        if (maxX <= slotStartX)
            return slotStartX;
        float x = toggleCenterX - textWidth * 0.5f;
        if (x < slotStartX)
            x = slotStartX;
        if (x > maxX)
            x = maxX;
        return x;
    };

    ImFont* valueFont = SettingsRightValueFont();
    auto renderValueText = [&](const char* text, bool disabledText) {
        const char* safeText = text != nullptr ? text : "";
        const float textWidth = valueFont->CalcTextSizeA(valueFont->LegacySize, FLT_MAX, -1.f, safeText).x;
        ImGui::SetCursorPosX(centeredValueX(textWidth));
        ImGui::PushFont(valueFont);
        if (disabledText)
            ImGui::TextDisabled("%s", safeText);
        else
            ImGui::TextUnformatted(safeText);
        ImGui::PopFont();
    };

    if (!cfg.disabled && cfg.valueClickable) {
        ImGui::PushFont(valueFont);
        if (ImGui::Selectable(displayValue.c_str(), false, ImGuiSelectableFlags_None, ImVec2(valueWidth, 0.0f)))
            ImGui::OpenPopup(cfg.popupID);
        ImGui::PopFont();
    } else if (cfg.disabled) {
        renderValueText(displayValue.c_str(), true);
    } else {
        renderValueText(displayValue.c_str(), false);
    }

    // Render popup
    bool valueChanged = false;

    // Configure popup window BEFORE opening (DuckStation-style)
    centerNextWindow();
    float maxWidth = ImGui::GetIO().DisplaySize.x * 0.5f;
    float maxHeight = ImGui::GetIO().DisplaySize.y * 0.7f;

    // Set fixed width for DuckStation-style choice dialog
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(uiScaled(Layout::POPUP_MIN_WIDTH), 0),
        ImVec2(maxWidth, maxHeight)
    );
    ImGui::SetNextWindowSize(ImVec2(uiScaled(Layout::POPUP_WIDTH), 0), ImGuiCond_Always);

    PopupStyleScope style;
    if (ImGui::BeginPopup(cfg.popupID, ImGuiWindowFlags_NoScrollbar)) {
        const char* popupTitle = (cfg.title != nullptr && cfg.title[0] != '\0') ? cfg.title : rowLabel;
        const char* popupDescription = (cfg.description != nullptr && cfg.description[0] != '\0') ? cfg.description : cfg.tooltip;
        const bool hasHeader = (popupTitle != nullptr && popupTitle[0] != '\0')
                            || (popupDescription != nullptr && popupDescription[0] != '\0');

        if (popupTitle != nullptr && popupTitle[0] != '\0') {
            ImGui::PushFont(largeFont);
            ImGui::TextUnformatted(popupTitle);
            ImGui::PopFont();
        }
        if (popupDescription != nullptr && popupDescription[0] != '\0') {
            ImGui::PushFont(SettingsDescriptionFont());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(popupDescription);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        if (hasHeader && cfg.showHeaderDivider) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // DuckStation-style menu button layout
        {
            BeginMenuButtons menuButtons;

            for (int i = 0; i < cfg.optionCount; i++) {
                int storageIdx = cfg.storageIndexMap ? cfg.storageIndexMap(i) : i;
                bool isSelected = (*cfg.currentValue == storageIdx);

                // Render selectable with proper sizing
                ImVec2 buttonSize(ImGui::GetContentRegionAvail().x, uiScaled(Layout::MENU_BUTTON_HEIGHT));
                auto onSelected = [&]() {
                    if (ImGui::Selectable(cfg.options[i], isSelected, ImGuiSelectableFlags_DontClosePopups, buttonSize)) {
                        *cfg.currentValue = storageIdx;
                        valueChanged = true;

                        bool shouldClose = true;
                        if (cfg.onChange) {
                            shouldClose = cfg.onChange(storageIdx);
                        }
                        if (shouldClose) {
                            ImGui::CloseCurrentPopup();
                        }
                    }
                };

                // Highlight selected item with the active row style.
                if (isSelected) {
                    ImguiStyleColor selectColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                    ImguiStyleColor hoverColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                    onSelected();
                } else {
                    onSelected();
                }

                // Set default focus on selected item
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        } // End menu buttons

        ImGui::EndPopup();
    }

    return valueChanged;
}

// Internal helper to render slider popup (DuckStation-style)
bool RenderSliderPopup(PopupSliderConfig& cfg)
{
    const auto formatValueText = [&](int value, char* out, size_t outSize) {
        if (cfg.valueFormatter) {
            const std::string text = cfg.valueFormatter(value);
            snprintf(out, outSize, "%s", text.c_str());
        } else {
            snprintf(out, outSize, cfg.format, value);
        }
    };

    // Icon and label
    SettingIcon(cfg.icon, ImVec2(uiScaled(cfg.iconSize), uiScaled(cfg.iconSize)));
    ImGui::SameLine(0, uiScaled(cfg.iconSpacing));
    const char* rowLabel = cfg.label ? cfg.label : "";
    ImGui::PushFont(largeFont);
    ImGui::TextUnformatted(rowLabel);
    ImGui::PopFont();

    // Current value display (centered toward toggle column)
    const float valueWidth = uiScaled(cfg.valueWidth);
    const float valueRightPadding = uiScaled(Layout::POPUP_VALUE_RIGHT_PADDING);
    const float rowRightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float slotStartX = rowRightX - valueWidth - valueRightPadding;
    const float toggleCenterX = rowRightX - uiScaled(28.0f) - uiScaled(50.0f) * 0.5f;
    const float minRightMargin = uiScaled(16.0f);
    ImGui::SameLine(slotStartX);
    if (cfg.valueVerticalOffset != 0.0f) {
        const ImVec2 pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(pos.x, pos.y + cfg.valueVerticalOffset));
    }

    char currentValue[32];
    formatValueText(*cfg.currentValue, currentValue, sizeof(currentValue));

    // Display current value as centered text (full row handles popup opening)
    ImFont* valueFont = SettingsRightValueFont();
    const float currentValueWidth = valueFont->CalcTextSizeA(valueFont->LegacySize, FLT_MAX, -1.f, currentValue).x;
    const float maxX = rowRightX - minRightMargin - currentValueWidth;
    float centeredX = toggleCenterX - currentValueWidth * 0.5f;
    if (maxX <= slotStartX)
        centeredX = slotStartX;
    else
    {
        if (centeredX < slotStartX)
            centeredX = slotStartX;
        if (centeredX > maxX)
            centeredX = maxX;
    }
    ImGui::SetCursorPosX(centeredX);
    ImGui::PushFont(valueFont);
    ImGui::TextUnformatted(currentValue);
    ImGui::PopFont();

    // Render popup
    bool valueChanged = false;

    // Configure popup window BEFORE opening (DuckStation-style)
    centerNextWindow();
    float maxWidth = ImGui::GetIO().DisplaySize.x * 0.85f;
    float maxHeight = ImGui::GetIO().DisplaySize.y * 0.7f;
    // Keep the popup wide enough for the centered slider and range labels
    // without forcing every slider dialog to open at an oversized fixed width.
    const float popupWidth = std::min(std::max(uiScaled(320.0f), uiScaled(cfg.sliderWidth + 48.0f)), maxWidth);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(popupWidth, maxWidth), 0), ImVec2(maxWidth, maxHeight));
    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0), ImGuiCond_Always);

    PopupStyleScope style;
    if (ImGui::BeginPopup(cfg.popupID, ImGuiWindowFlags_NoScrollbar)) {
        if (rowLabel[0] != '\0') {
            ImGui::PushFont(largeFont);
            ImGui::TextUnformatted(rowLabel);
            ImGui::PopFont();
        }

        if (cfg.description) {
            ImGui::PushFont(SettingsDescriptionFont());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(cfg.description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Display value range and default value (DuckStation-style)
        char rangeText[128];
        snprintf(rangeText, sizeof(rangeText), "Value Range: %d - %d", cfg.minValue, cfg.maxValue);
        ImGui::PushFont(PopupEmphasisFont());
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        ImGui::TextDisabled("%s", rangeText);

        if (cfg.defaultValue >= cfg.minValue && cfg.defaultValue <= cfg.maxValue) {
            char defaultText[128];
            snprintf(defaultText, sizeof(defaultText), "Default Value: %d", cfg.defaultValue);
            const float rangeWidth = ImGui::CalcTextSize(rangeText).x;
            const float defaultWidth = ImGui::CalcTextSize(defaultText).x;
            const float sameLineSpacing = uiScaled(20);
            if (rangeWidth + sameLineSpacing + defaultWidth <= contentWidth)
                ImGui::SameLine(0, sameLineSpacing);
            ImGui::TextDisabled("%s", defaultText);
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // DuckStation-style rounded slider with no border
        const float frameRounding = 20.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(frameRounding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, uiScaled(frameRounding));

        float sliderWidth = uiScaled(cfg.sliderWidth);
        ImVec2 sliderPos = ImGui::GetCursorScreenPos();
        const ImGuiID popupId = ImGui::GetID(cfg.popupID);
        static ImGuiID modePopupId = 0;
        static bool modeTextEntry = false;
        static bool modeControllerSlider = false;
        static bool modeFocusTextEntry = false;
        static bool modePendingChanges = false;
        static int modeTypedValue = 0;

        const bool popupAppearing = ImGui::IsWindowAppearing();
        if (popupAppearing)
        {
            ImGuiContext& g = *GImGui;
            const bool controllerOpen = (g.NavInputSource == ImGuiInputSource_Gamepad);
            modePopupId = popupId;
            // Mouse opens use the draggable slider. Controller opens use slider-only
            // interaction and accepts with A. Typing switches mouse/keyboard to text entry.
            modeTextEntry = cfg.preferTextEntry;
            modeControllerSlider = controllerOpen && !modeTextEntry;
            modeFocusTextEntry = modeTextEntry;
            modePendingChanges = cfg.hasPendingChanges || (cfg.showApplyFlag && *cfg.showApplyFlag);
            modeTypedValue = *cfg.currentValue;
        }

        const auto hasTextInput = [] {
            ImGuiIO& io = ImGui::GetIO();
            for (int i = 0; i < io.InputQueueCharacters.Size; i++)
            {
                const ImWchar c = io.InputQueueCharacters[i];
                if ((c >= '0' && c <= '9') || c == '-' || c == '+')
                    return true;
            }
            return false;
        };

        if (modePopupId == popupId && !modeControllerSlider && !modeTextEntry && hasTextInput())
        {
            modeTextEntry = true;
            modeFocusTextEntry = true;
            modeTypedValue = *cfg.currentValue;
        }

        const bool textEntryMode = (modePopupId == popupId) && modeTextEntry;
        bool requestFocusApply = false;
        bool controllerAcceptValue = false;
        bool keyboardAcceptValue = false;

        if (textEntryMode)
        {
            char sliderValueText[32];
            formatValueText(modeTypedValue, sliderValueText, sizeof(sliderValueText));
            const ImVec2 valueTextSize = ImGui::CalcTextSize(sliderValueText);
            const float inputWidth = std::max(uiScaled(84.0f), valueTextSize.x + uiScaled(18.0f));
            ImGui::SetCursorScreenPos(ImVec2(
                sliderPos.x + (sliderWidth - inputWidth) * 0.5f,
                sliderPos.y
            ));
            ImGui::SetNextItemWidth(inputWidth);
            ImGui::PushID("SliderTypedValue");
            if (modeFocusTextEntry)
            {
                ImGui::SetKeyboardFocusHere();
                modeFocusTextEntry = false;
            }
            const bool accept = ImGui::InputInt("##TypedValue", &modeTypedValue, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::PopID();

            if (accept)
            {
                modeTypedValue = std::clamp(modeTypedValue, cfg.minValue, cfg.maxValue);
                if (modeTypedValue != *cfg.currentValue)
                {
                    *cfg.currentValue = modeTypedValue;
                    valueChanged = true;
                    if (cfg.onValueChange)
                        cfg.onValueChange();
                }
                if (cfg.onApply)
                    cfg.onApply();
                cfg.hasPendingChanges = false;
                modePendingChanges = false;
                if (cfg.showApplyFlag)
                    *cfg.showApplyFlag = false;
                ImGui::CloseCurrentPopup();
            }
        }
        else
        {
            ImGui::SetNextItemWidth(sliderWidth);
            if (popupAppearing)
                ImGui::SetKeyboardFocusHere();

            int tempValue = *cfg.currentValue;
            const bool sliderChanged = ImGui::SliderInt(
                "##SliderPopup",
                &tempValue,
                cfg.minValue,
                cfg.maxValue,
                "",
                ImGuiSliderFlags_NoInput
            );
            const bool sliderFocused = ImGui::IsItemFocused();
            const bool sliderActive = ImGui::IsItemActive();
            if (modeControllerSlider && (ImGui::IsItemHovered() || sliderActive) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                modeControllerSlider = false;

            if (sliderFocused)
            {
                controllerAcceptValue = modePopupId == popupId
                    && modeControllerSlider
                    && !popupAppearing
                    && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);

                int navDelta = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)
                    || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)
                    || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true))
                    navDelta = -1;
                else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)
                         || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)
                         || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true))
                    navDelta = 1;

                if (navDelta != 0)
                {
                    tempValue = std::clamp(tempValue + navDelta, cfg.minValue, cfg.maxValue);
                    if (tempValue != *cfg.currentValue)
                    {
                        *cfg.currentValue = tempValue;
                        valueChanged = true;
                        if (!modeControllerSlider && cfg.showApplyFlag)
                            *cfg.showApplyFlag = true;
                        cfg.hasPendingChanges = true;
                        modePendingChanges = true;
                        if (cfg.onValueChange)
                            cfg.onValueChange();
                    }
                }

                const bool navDown = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)
                                  || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false)
                                  || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false);
                const bool navUp = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)
                                || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)
                                || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false);
                if (sliderActive && (navDown || navUp))
                {
                    ImGui::ClearActiveID();
                    requestFocusApply = navDown;
                }
            }

            if (sliderChanged) {
                *cfg.currentValue = tempValue;
                valueChanged = true;

                // Update state tracking
                if (!modeControllerSlider && cfg.showApplyFlag) {
                    *cfg.showApplyFlag = true;
                }
                cfg.hasPendingChanges = true;
                modePendingChanges = true;

                if (cfg.onValueChange) {
                    cfg.onValueChange();
                }
            }
            keyboardAcceptValue = !modeControllerSlider
                && (cfg.hasPendingChanges || modePendingChanges || (cfg.showApplyFlag && *cfg.showApplyFlag))
                && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));

            char sliderValueText[32];
            formatValueText(*cfg.currentValue, sliderValueText, sizeof(sliderValueText));
            const ImVec2 valueTextSize = ImGui::CalcTextSize(sliderValueText);
            const ImVec2 sliderMin = ImGui::GetItemRectMin();
            const ImVec2 sliderMax = ImGui::GetItemRectMax();
            const ImVec2 valueTextPos(
                sliderMin.x + (sliderMax.x - sliderMin.x - valueTextSize.x) * 0.5f,
                sliderMin.y + (sliderMax.y - sliderMin.y - valueTextSize.y) * 0.5f
            );
            ImGui::GetWindowDrawList()->AddText(valueTextPos, ImGui::GetColorU32(ImGuiCol_Text), sliderValueText);
        }

        ImGui::PopStyleVar(3); // Pop FrameRounding, FrameBorderSize, GrabRounding

        ImGui::Spacing();

        // DuckStation-style button layout (right-aligned)
        bool shouldShowApply = !modeControllerSlider &&
                               (cfg.hasPendingChanges || modePendingChanges || (cfg.showApplyFlag && *cfg.showApplyFlag));

        if (controllerAcceptValue || keyboardAcceptValue) {
            if (cfg.onApply)
                cfg.onApply();
            // Live sliders have already applied through onValueChange, but accepting
            // should still clear pending state and close the popup.
            cfg.hasPendingChanges = false;
            modePendingChanges = false;
            if (cfg.showApplyFlag)
                *cfg.showApplyFlag = false;
            ImGui::CloseCurrentPopup();
        } else if (cfg.onApply && shouldShowApply) {
            // Begin menu buttons container
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(8), uiScaled(8)));

            const float buttonWidth = uiScaled(120);
            const float buttonHeight = uiScaled(32);

            // Right-align buttons
            float availableWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(availableWidth - buttonWidth);

            // Apply button with rounded corners
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(uiScaled(16), uiScaled(8)));

            if (requestFocusApply)
                ImGui::SetKeyboardFocusHere();

            if (ImGui::Button(cfg.applyButtonText, ImVec2(buttonWidth, buttonHeight))) {
                cfg.onApply();
                cfg.hasPendingChanges = false; // Reset state
                modePendingChanges = false;
                if (cfg.showApplyFlag) {
                    *cfg.showApplyFlag = false; // Reset external flag
                }
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopStyleVar(3); // Pop ItemSpacing, FrameRounding, FramePadding
        }

        ImGui::EndPopup();
    }

    return valueChanged;
}

} // namespace Detail

// Main entry point
bool SettingPopup(PopupConfig& config)
{
    switch (config.type) {
        case PopupType::Options:
            return Detail::RenderOptionsPopup(config.options);
        case PopupType::Slider:
            return Detail::RenderSliderPopup(config.slider);
        default:
            return false;
    }
}

// Convenience overload for options
bool SettingPopup(
    const char* label,
    const char* icon,
    const char* popupID,
    const char* const* options,
    int optionCount,
    int* currentValue,
    bool disabled,
    const char* disabledPrefix)
{
    PopupConfig cfg;
    cfg.type = PopupType::Options;
    cfg.options.label = label;
    cfg.options.icon = icon;
    cfg.options.popupID = popupID;
    cfg.options.options = options;
    cfg.options.optionCount = optionCount;
    cfg.options.currentValue = currentValue;
    cfg.options.disabled = disabled;
    cfg.options.disabledPrefix = disabledPrefix;
    return SettingPopup(cfg);
}

// Convenience overload for slider
bool SettingPopup(
    const char* label,
    const char* icon,
    const char* popupID,
    const char* description,
    int* currentValue,
    int minValue,
    int maxValue,
    const char* format,
    std::function<void()> onApply)
{
    PopupConfig cfg;
    cfg.type = PopupType::Slider;
    cfg.slider.label = label;
    cfg.slider.icon = icon;
    cfg.slider.popupID = popupID;
    cfg.slider.description = description;
    cfg.slider.currentValue = currentValue;
    cfg.slider.minValue = minValue;
    cfg.slider.maxValue = maxValue;
    cfg.slider.format = format;
    cfg.slider.onApply = std::move(onApply);
    return SettingPopup(cfg);
}

} // namespace SettingsUI
