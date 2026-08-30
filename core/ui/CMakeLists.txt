#
#	Portions Copyright 2026 The Hollycast Authors
#
#	This file is part of Hollycast.
#
#     Hollycast is free software: you can redistribute it and/or modify
#     it under the terms of the GNU General Public License as published by
#     the Free Software Foundation, either version 2 of the License, or
#     (at your option) any later version.
#
#     Hollycast is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     GNU General Public License for more details.
#
#     You should have received a copy of the GNU General Public License
#     along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
#

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            game_scanner.cpp
            game_scanner.h
            imgui_driver.h
            gui.cpp
            gui.h
            gui_font.cpp
            gui_font.h
            gui_menu.cpp
            gui_menu.h
            gui_achievements.cpp
            vgamepad.cpp
            vgamepad.h
            gui_chat.h
            gui_cheats.cpp
            gui_util.cpp
            gui_util.h
            mainui.cpp
            mainui.h
            settings.cpp
            settings.h
            settings_about.cpp
            settings_audio.cpp
            settings_controls.cpp
            settings_general.cpp
            settings_network.cpp
            settings_video.cpp
            settings_new.cpp
            settings_chd.cpp
            settings_new.h
            video_preset.cpp
            video_preset.h
            boxart/arcade_scraper.h
            boxart/boxart.cpp
            boxart/boxart.h
            boxart/gamesdb.cpp
            boxart/gamesdb.h
            boxart/pvrparser.h
            boxart/scraper.cpp
            boxart/scraper.h
            boxart/vmu_icon.cpp
            boxart/vmu_icon.h)

    if(USE_DISCORD)
        target_sources(${PROJECT_NAME} PRIVATE discord.cpp)
    endif()
endif()
