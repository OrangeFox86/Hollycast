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
#include "dreamlink_android_gamepad.h"

#include "dreampicoport_android_gamepad.h"

#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "ui/gui.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include <cfg/option.h>
#include <iomanip>
#include <sstream>
#include <optional>
#include <thread>
#include <list>
#include <mutex>
#include <condition_variable>
#include <atomic>

#if defined(__linux__) || (defined(__APPLE__) && defined(TARGET_OS_MAC))
#include <dirent.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#endif

bool DreamLinkAndroidGamepad::isDreamLinkGamepad(int vid, int pid)
{
	if (DreamPicoPortAndroidGamepad::identify(vid, pid))
		return true;
	return false;
}

bool DreamLinkAndroidGamepad::isPermissionRequired(int vid, int pid)
{
	if (DreamPicoPortAndroidGamepad::identify(vid, pid))
		return true;
	return false;
}

DreamLinkAndroidGamepad::DreamLinkAndroidGamepad(
	std::shared_ptr<GamepadDreamLink> dreamlink,
	int maple_port,
	const AndroidGamepadDevice::AndroidJoystickData& joystickData
)
	: AndroidGamepadDevice(maple_port, joystickData), dreamlink(std::move(dreamlink))
{}

const char* DreamLinkAndroidGamepad::status()
{
	if (!dreamlink)
	{
		return i18n::T("Connection Failed");
	}

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

void DreamLinkAndroidGamepad::set_maple_port(int port)
{
	int oldPort = maple_port();
	if (oldPort == port)
		return;

	AndroidGamepadDevice::set_maple_port(port);

	if (dreamlink)
	{
		dreamlink->changeBus(port);
	}
}

void DreamLinkAndroidGamepad::registered()
{
	AndroidGamepadDevice::registered();
	if (dreamlink)
	{
		dreamlink->registered();
	}
}

void DreamLinkAndroidGamepad::resetMappingToDefault(bool arcade, bool gamepad) {
	AndroidGamepadDevice::resetMappingToDefault(arcade, gamepad);
	if (input_mapper) {
		setCustomMapping(input_mapper);
		setBaseDefaultMapping(input_mapper);
	}
}

void DreamLinkAndroidGamepad::close(JNIEnv *env)
{
	AndroidGamepadDevice::close(env);
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
}

std::shared_ptr<InputMapping> DreamLinkAndroidGamepad::getDefaultMapping() {
	std::shared_ptr<InputMapping> mapping = AndroidGamepadDevice::getDefaultMapping();
	if (mapping) {
		setCustomMapping(mapping);
		setBaseDefaultMapping(mapping);
	}
	return mapping;
}

void DreamLinkAndroidGamepad::setBaseDefaultMapping(const std::shared_ptr<InputMapping>& mapping) const
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

std::shared_ptr<DreamLinkAndroidGamepad> createDreamLinkAndroidGamepad(
	JNIEnv *env,
	jobject usbManager,
	int maple_port,
	const AndroidGamepadDevice::AndroidJoystickData& joystickData
)
{
	if (DreamPicoPortAndroidGamepad::identify(joystickData.vid, joystickData.pid))
		return std::make_shared<DreamPicoPortAndroidGamepad>(env, usbManager, maple_port, joystickData);
	return nullptr;
}
