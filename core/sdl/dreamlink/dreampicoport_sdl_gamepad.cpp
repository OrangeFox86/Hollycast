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

#include "dreampicoport_sdl_gamepad.h"

#include "input/dreamlink/maplelink.h"
#include "input/dreamlink/maplelinkregistry.h"
#include "input/dreamlink/dreampicoport.h"

#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "ui/gui.h"
#include "cfg/option.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include "log/Log.h"
#include "emulator.h"

#include "DreamPicoPortApi.hpp"

// C++ standard library
#include <iomanip>
#include <sstream>
#include <thread>
#include <list>
#include <vector>
#include <array>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <string_view>

#ifndef TARGET_UWP
#include <asio.hpp>
#endif

#if defined(__linux__) || (defined(__APPLE__) && defined(TARGET_OS_MAC))
#include <dirent.h>
#endif

#if defined(_WIN32) && !defined(TARGET_UWP)
#include <windows.h>
#include <setupapi.h>
#endif

//! Only to be called during instantiation to determine hardware information
//! @param[in] joystick_idx SDL joystick index
//! @param[in] sdl_joystick SDL joystick object
static DreamPicoPort::HardwareInfo parse_hw_info(int joystick_idx, SDL_Joystick* sdl_joystick) {
#if defined(_WIN32)
	// Workaround: Getting the instance ID here fixes some sort of L/R trigger bug in Windows dinput for some reason
	(void)SDL_JoystickGetDeviceInstanceID(joystick_idx);
#endif

	DreamPicoPort::HardwareInfo hw_info;

	// The number of buttons gives a clue as to what index the controller is
	int nbuttons = SDL_JoystickNumButtons(sdl_joystick);

	if (nbuttons >= 28 && nbuttons <= 31) {
		hw_info.hardware_bus = 31 - nbuttons;
		hw_info.is_single_device = false;
	}
	else {
		hw_info.hardware_bus = 0;
		hw_info.is_single_device = true;
	}

	// Set the serial number if found by SDL Joystick
	const char* joystick_serial = SDL_JoystickGetSerial(sdl_joystick);
	if (joystick_serial) {
		// Will normally reach here on Linux systems but not Windows or macOS
		hw_info.serial_number = joystick_serial;
	}

	// Windows will cache the joystick name, so it's not a good idea to check SDL_JoystickName() on Windows
#if !defined(_WIN32)
	if (hw_info.serial_number.empty()) {
		// Version 1.2.0 and later embeds serial in name as a workaround for MacOS and Linux
		// Serial is expected between a dash (-) and space ( ) character or until end of string
		// Will normally reach here on macOS systems
		const char* joystick_name = SDL_JoystickName(sdl_joystick);
		if (joystick_name) {
			hw_info.serial_number = DreamPicoPort::getSerialFromName(joystick_name);
		}
	}
#endif

#if defined(_WIN32)

	// These extra checks are necessary for Windows because it likes to hold onto old joystick names, before the serial
	// was embedded in it, and SDL_JoystickGetSerial() will have previously failed.
	if (hw_info.serial_number.empty()) {
		// This only works in Windows because the joystick_path is not given in other OSes
		const char* joystick_path = SDL_JoystickPath(sdl_joystick);

		struct SDL_hid_device_info* devs = SDL_hid_enumerate(DreamPicoPort::VID, DreamPicoPort::PID);
		if (devs) {
			struct SDL_hid_device_info* my_dev = nullptr;

			if (!devs->next) {
				// Only single device found, so this is simple
				my_dev = devs;
			} else {
				struct SDL_hid_device_info* it = devs;

				if (joystick_path)
				{
					while (it)
					{
						// Note: hex characters will be differing case, so case-insensitive cmp is needed
						if (it->path && 0 == SDL_strcasecmp(it->path, joystick_path)) {
							my_dev = it;
							break;
						}
						it = it->next;
					}
				}
			}

			// Set serial number if found in SDL_hid
			if (my_dev && my_dev->serial_number) {
				int len = WideCharToMultiByte(CP_UTF8, 0, my_dev->serial_number, -1, nullptr, 0, nullptr, nullptr);
				if (len > 0) {
					std::vector<char> buffer(len);
					WideCharToMultiByte(CP_UTF8, 0, my_dev->serial_number, -1, buffer.data(), len, nullptr, nullptr);
					hw_info.serial_number = std::string(buffer.data());
				}
			}

			SDL_hid_free_enumeration(devs);
		}
	}

#endif // #if defined(_WIN32)

	return hw_info;
}

DreamPicoPortSDLGamepad::DreamPicoPortSDLGamepad(
	int maple_port,
	int joystick_idx,
	SDL_Joystick* sdl_joystick
) :
	DreamLinkSDLGamepad(
		std::make_shared<DreamPicoPort>(maple_port, parse_hw_info(joystick_idx, sdl_joystick)),
		maple_port,
		joystick_idx,
		sdl_joystick
)
{
	DreamPicoPort *picoPort = dynamic_cast<DreamPicoPort*>(dreamlink.get());
	assert(picoPort != nullptr);
	_name = picoPort->getName();

	const DreamPicoPort::HardwareInfo& hw_info = picoPort->getHardwareInfo();

	if (!hw_info.serial_number.empty()) {
		// Ensure this is ordered by SDL, product name, serial, and port char
		_sort_id = (
			std::string("sdl_") +
			hw_info.getProductName() + std::string("_") +
			hw_info.serial_number + std::string("_") +
			std::string(1, hw_info.getPortChar())
		);
		// Locking to name, which includes A-D, plus serial number will ensure correct enumeration every time
		_unique_id = std::string("sdl_") + hw_info.getName("", true) + std::string("_") + hw_info.serial_number;
		// Reload mapping now that unique ID changed
		loadMapping();
	}

	int bus = picoPort->getDefaultBus();
	if (DreamLink::isValidBus(bus))
		set_maple_port(bus);
}

bool DreamPicoPortSDLGamepad::identify(int deviceIndex)
{
	char guid_str[33] {};
	SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(deviceIndex), guid_str, sizeof(guid_str));
	// Dreamcast Controller USB VID:1209 PID:2f07
	const char* pid_vid_guid_str = guid_str + 8;
	if (memcmp(VID_PID_GUID, pid_vid_guid_str, 16) == 0) {
		return true;
	}
	return false;
}

void DreamPicoPortSDLGamepad::setCustomMapping(const std::shared_ptr<InputMapping>& mapping)
{
	// Since this is a real DC controller, no deadzone adjustment is needed
	mapping->dead_zone = 0.0f;
	// Map the things not set by SDL
	mapping->set_button(DC_BTN_C, static_cast<u32>(ButtonCode::C));
	mapping->set_button(DC_BTN_Z, static_cast<u32>(ButtonCode::Z));
	mapping->set_button(DC_BTN_D, static_cast<u32>(ButtonCode::D));
	mapping->set_button(DC_DPAD2_UP, static_cast<u32>(ButtonCode::UP_B));
	mapping->set_button(DC_DPAD2_DOWN, static_cast<u32>(ButtonCode::DOWN_B));
	mapping->set_button(DC_DPAD2_LEFT, static_cast<u32>(ButtonCode::LEFT_B));
	mapping->set_button(DC_DPAD2_RIGHT, static_cast<u32>(ButtonCode::RIGHT_B));
}

const char *DreamPicoPortSDLGamepad::get_button_name(u32 code)
{
	using namespace i18n;
	switch (code) {
		// Coincides with buttons setup in setCustomMapping
		case static_cast<u32>(ButtonCode::C): return "C";
		case static_cast<u32>(ButtonCode::Z): return "Z";
		case static_cast<u32>(ButtonCode::D): return "D";
		case static_cast<u32>(ButtonCode::UP_B): return T("DPad2 Up");
		case static_cast<u32>(ButtonCode::DOWN_B): return T("DPad2 Down");
		case static_cast<u32>(ButtonCode::LEFT_B): return T("DPad2 Left");
		case static_cast<u32>(ButtonCode::RIGHT_B): return T("DPad2 Right");

		// Alternate directional buttons
		case static_cast<u32>(ButtonCode::ALT_UP): return T("Alt Up");
		case static_cast<u32>(ButtonCode::ALT_DOWN): return T("Alt Down");
		case static_cast<u32>(ButtonCode::ALT_LEFT): return T("Alt Left");
		case static_cast<u32>(ButtonCode::ALT_RIGHT): return T("Alt Right");

		// These buttons are normally not physically accessible but are mapped on DreamPicoPort
		case static_cast<u32>(ButtonCode::VMU1_BUTTON_A): return T("VMU1 A");
		case static_cast<u32>(ButtonCode::VMU1_BUTTON_B): return T("VMU1 B");
		case static_cast<u32>(ButtonCode::VMU1_BUTTON_UP): return T("VMU1 Up");
		case static_cast<u32>(ButtonCode::VMU1_BUTTON_DOWN): return T("VMU1 Down");
		case static_cast<u32>(ButtonCode::VMU1_BUTTON_LEFT): return T("VMU1 Left");
		case static_cast<u32>(ButtonCode::VMU1_BUTTON_RIGHT): return T("VMU1 Right");

		case static_cast<u32>(ButtonCode::CHANGE_EVENT): return T("Device Change");

		default: return DreamLinkSDLGamepad::get_button_name(code); // use the default name
	}
}

bool DreamPicoPortSDLGamepad::gamepad_btn_input(u32 code, bool pressed)
{
	if (code == static_cast<u32>(ButtonCode::CHANGE_EVENT) && !pressed)
	{
		DreamPicoPort *picoPort = dynamic_cast<DreamPicoPort*>(dreamlink.get());
		if (picoPort)
		{
			picoPort->queryPeripherals(false);
		}
	}

	return DreamLinkSDLGamepad::gamepad_btn_input(code, pressed);
}
