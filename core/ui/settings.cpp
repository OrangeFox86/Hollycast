/*
	Copyright 2025 flyinghead

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
#include "settings.h"
#include "settings_new.h"
#include "log/Log.h"

#include <cstdlib>

namespace {

SettingsNew::SettingsTab toSettingsNewTab(GuiSettingsTab tab)
{
	switch (tab)
	{
	case GuiSettingsTab::General:
		return SettingsNew::SettingsTab::General;
	case GuiSettingsTab::Library:
		return SettingsNew::SettingsTab::Library;
	case GuiSettingsTab::Video:
		return SettingsNew::SettingsTab::Video;
	case GuiSettingsTab::Audio:
		return SettingsNew::SettingsTab::Audio;
	case GuiSettingsTab::Controls:
		return SettingsNew::SettingsTab::Controls;
	case GuiSettingsTab::Network:
		return SettingsNew::SettingsTab::Network;
	case GuiSettingsTab::Advanced:
		return SettingsNew::SettingsTab::Advanced;
	case GuiSettingsTab::About:
		return SettingsNew::SettingsTab::About;
	default:
		ERROR_LOG(RENDERER, "Unknown GuiSettingsTab value: %d", static_cast<int>(tab));
	    std::abort();
	}
}

} // namespace

// Legacy compatibility entry point. The full settings screen is implemented in
// settings_new.cpp; preserve this symbol for compatibility and merge stability.
void gui_display_settings()
{
	SettingsNew::renderSettingsNew();
}

void gui_prepare_settings_tab(GuiSettingsTab tab)
{
	SettingsNew::openTab(toSettingsNewTab(tab));
}

void gui_reset_settings_view()
{
	SettingsNew::resetState();
}

void gui_focus_boxart_settings_section()
{
	SettingsNew::focusBoxArtSection();
}
