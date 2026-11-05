/*
	Copyright 2024 flyinghead
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

#ifdef USE_DREAMLINK_DEVICES

#include "dreamlink_sdl_gamepad.h"

#include <memory>

//! See: https://github.com/OrangeFox86/DreamPicoPort

class DreamPicoPortSDLGamepad : public DreamLinkSDLGamepad
{
public:
	//! Enumerates the raw button codes in SDL for DreamPicoPort
	enum class ButtonCode : s32
	{
		A = 0,
		B = 1,
		C = 2,
		X = 3,
		Y = 4,
		Z = 5,
		RIGHT_B = 6,
		LEFT_B = 7,
		DOWN_B = 8,
		UP_B = 9,
		D = 10,
		START = 11,
		VMU1_BUTTON_A = 12,
		// UNASSIGNED = 13,
		// UNASSIGNED = 14,
		VMU1_BUTTON_B = 15,
		VMU1_BUTTON_UP = 16,
		VMU1_BUTTON_DOWN = 17,
		VMU1_BUTTON_LEFT = 18,
		VMU1_BUTTON_RIGHT = 19,
		CHANGE_EVENT = 20,
		// UNASSIGNED = 21,
		// UNASSIGNED = 22,
		// UNASSIGNED = 23,
		ALT_UP = 24,
		ALT_DOWN = 25,
		ALT_LEFT = 26,
		ALT_RIGHT = 27,
		PLAYER_4 = 28,
		PLAYER_3 = 29,
		PLAYER_2 = 30,
		PLAYER_1 = 31
	};

	//! Enumerates the raw axis codes in SDL for DreamPicoPort
	enum class AxisCode : s32
	{
		LEFT_X = 0,
		LEFT_Y = 1,
		RIGHT_X = 2,
		RIGHT_Y = 3,
		LEFT_TRIGGER = 4,
		RIGHT_TRIGGER = 5
	};

public:
	DreamPicoPortSDLGamepad(int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick);
	~DreamPicoPortSDLGamepad() = default;
	const char *get_button_name(u32 code) override;
	static bool identify(int deviceIndex);

	inline const std::string& sort_id() override
	{
		return !_sort_id.empty() ? _sort_id : DreamLinkSDLGamepad::sort_id();
	}

	bool gamepad_btn_input(u32 code, bool pressed) override;

protected:
	void setCustomMapping(const std::shared_ptr<InputMapping>& mapping) override;

protected:
	//! Dreamcast Controller USB VID:1209 PID:2f07
	static constexpr const char* VID_PID_GUID = "09120000072f0000";

private:
	//! ID used for sorting on the UI
	std::string _sort_id;
};
#endif // USE_DREAMLINK_DEVICES
