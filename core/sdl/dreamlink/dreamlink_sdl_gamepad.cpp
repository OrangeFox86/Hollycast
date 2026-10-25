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
#include "dreamlink_sdl_gamepad.h"

#include "dreamconn_sdl_gamepad.h"
#include "dreampicoport_sdl_gamepad.h"

#include "hw/maple/maple_devs.h"
#include "ui/gui.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include <cfg/option.h>
#include <SDL.h>
#include <memory>

#if defined(__linux__) || (defined(__APPLE__) && defined(TARGET_OS_MAC))
#include <dirent.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#endif

bool DreamLinkSDLGamepad::isDreamLinkGamepad(int deviceIndex)
{
	char guid_str[33] {};
	SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(deviceIndex), guid_str, sizeof(guid_str));
	NOTICE_LOG(INPUT, "GUID: %s VID:%c%c%c%c PID:%c%c%c%c", guid_str,
			guid_str[10], guid_str[11], guid_str[8], guid_str[9],
			guid_str[18], guid_str[19], guid_str[16], guid_str[17]);

#ifdef USE_DREAMCONN
	if (DreamConnSDLGamepad::identify(deviceIndex))
		return true;
#endif
	if (DreamPicoPortSDLGamepad::identify(deviceIndex))
		return true;
	return false;
}

DreamLinkSDLGamepad::DreamLinkSDLGamepad(std::shared_ptr<GamepadDreamLink> dreamlink, int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick)
	: SDLGamepad(maple_port, joystick_idx, sdl_joystick), dreamlink(dreamlink)
{
	verify(dreamlink != nullptr);
}

void DreamLinkSDLGamepad::close()
{
	if (dreamlink != nullptr)
	{
		const char* const name = dreamlink->getProductName();
		dreamlink->term();
		dreamlink.reset();

		if (!gui_is_open()) {
			if (!settings.network.online) {
				// Make sure settings are open in case disconnection happened mid-game
				gui_open_settings();
			} else {
				// While connected online, just pop up a toast
				char buffer[128];
				snprintf(buffer, sizeof(buffer), i18n::T("%s was disconnected"), name);
				os_notify(buffer, 6000);
			}
		}
	}
	SDLGamepad::close();
}

const char* DreamLinkSDLGamepad::status()
{
	if (dreamlink->isConnected())
	{
		return i18n::T("Connected");
	}

	const char* issueDesc = dreamlink->getIssueDescription();
	if (issueDesc)
	{
		return issueDesc;
	}

	return i18n::T("Disconnected");
}

void DreamLinkSDLGamepad::set_maple_port(int port)
{
	int oldPort = maple_port();
	if (oldPort == port)
		return;

	SDLGamepad::set_maple_port(port);

	dreamlink->changeBus(port);
}

void DreamLinkSDLGamepad::registered()
{
	SDLGamepad::registered();
	dreamlink->registered();
}

void DreamLinkSDLGamepad::resetMappingToDefault(bool arcade, bool gamepad) {
	SDLGamepad::resetMappingToDefault(arcade, gamepad);
	if (input_mapper) {
		setCustomMapping(input_mapper);
		setBaseDefaultMapping(input_mapper);
	}
}

std::shared_ptr<InputMapping> DreamLinkSDLGamepad::getDefaultMapping() {
	std::shared_ptr<InputMapping> mapping = SDLGamepad::getDefaultMapping();
	if (mapping) {
		setCustomMapping(mapping);
		setBaseDefaultMapping(mapping);
	}
	return mapping;
}

void DreamLinkSDLGamepad::setBaseDefaultMapping(const std::shared_ptr<InputMapping>& mapping) const
{
	const u32 leftTrigger = mapping->get_axis_code(maple_port(), DreamcastKey::DC_AXIS_LT).first;
	const u32 rightTrigger = mapping->get_axis_code(maple_port(), DreamcastKey::DC_AXIS_RT).first;
	const u32 startCode = mapping->get_button_code(maple_port(), DreamcastKey::DC_BTN_START);
	if (leftTrigger != InputMapping::InputDef::INVALID_CODE &&
		rightTrigger != InputMapping::InputDef::INVALID_CODE &&
		startCode != InputMapping::InputDef::INVALID_CODE)
	{
		mapping->set_button(DreamcastKey::EMU_BTN_MENU, InputMapping::ButtonCombo{
			InputMapping::InputSet{
				InputMapping::InputDef{leftTrigger, InputMapping::InputDef::InputType::AXIS_POS},
				InputMapping::InputDef{rightTrigger, InputMapping::InputDef::InputType::AXIS_POS},
				InputMapping::InputDef{startCode, InputMapping::InputDef::InputType::BUTTON}
			},
			false
		});
	}
}

std::shared_ptr<DreamLinkSDLGamepad> createDreamLinkSDLGamepad(int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick)
{
	if (DreamPicoPortSDLGamepad::identify(joystick_idx))
		return std::make_shared<DreamPicoPortSDLGamepad>(maple_port, joystick_idx, sdl_joystick);
#ifdef USE_DREAMCONN
	else if (DreamConnSDLGamepad::identify(joystick_idx))
		return std::make_shared<DreamConnSDLGamepad>(maple_port, joystick_idx, sdl_joystick);
#endif
	return nullptr;
}
