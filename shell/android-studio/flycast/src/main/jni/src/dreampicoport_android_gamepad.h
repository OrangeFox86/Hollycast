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
#pragma once

#include "dreamlink_android_gamepad.h"

#include <jni.h>

#include <memory>

class DreamPicoPortAndroidGamepad : public DreamLinkAndroidGamepad
{
public:
	//! Enumerates the raw button codes in Android for DreamPicoPort
	enum class ButtonCode : s32
	{
		A = AKEYCODE_BUTTON_A,
		B = AKEYCODE_BUTTON_B,
		C = AKEYCODE_BUTTON_C,
		X = AKEYCODE_BUTTON_X,
		Y = AKEYCODE_BUTTON_Y,
		Z = AKEYCODE_BUTTON_Z,
		RIGHT_B = AKEYCODE_BUTTON_L1,
		LEFT_B = AKEYCODE_BUTTON_R1,
		DOWN_B = AKEYCODE_BUTTON_L2,
		UP_B = AKEYCODE_BUTTON_R2,
		D = AKEYCODE_BUTTON_SELECT,
		START = AKEYCODE_BUTTON_START,
		VMU1_BUTTON_A = AKEYCODE_BUTTON_MODE,
		// The following come through as scan codes
		VMU1_BUTTON_B = -319,
		VMU1_BUTTON_UP = -704,
		VMU1_BUTTON_DOWN = -705,
		VMU1_BUTTON_LEFT = -706,
		VMU1_BUTTON_RIGHT = -707,
		CHANGE_EVENT = -708,
		ALT_UP = -712,
		ALT_DOWN = -713,
		ALT_LEFT = -714,
		ALT_RIGHT = -715
	};

	//! Enumerates the raw axis codes in Android for DreamPicoPort
	enum class AxisCode : s32
	{
		LEFT_X = AMOTION_EVENT_AXIS_X,
		LEFT_Y = AMOTION_EVENT_AXIS_Y,
		RIGHT_X = AMOTION_EVENT_AXIS_RX,
		RIGHT_Y = AMOTION_EVENT_AXIS_RY,
		LEFT_TRIGGER = AMOTION_EVENT_AXIS_Z,
		RIGHT_TRIGGER = AMOTION_EVENT_AXIS_RZ
	};

public:
	//! Constructor
	//! @param[in] env The local Java environment
	//! @param[in] usbManager A UsbManager object created from the current app context
	//! @param[in] maple_port The requested maple port index to use
	//! @param[in] joystickData All joystick data associated with the InputDevice
	DreamPicoPortAndroidGamepad(
		JNIEnv *env,
		jobject usbManager,
		int maple_port,
		const AndroidGamepadDevice::AndroidJoystickData& joystickData
	);

	//! Destructor
	~DreamPicoPortAndroidGamepad();

	//! Overridden from GamepadDevice
	//! Returns the locally-known button name for a given code
	const char *get_button_name(u32 code) override;

	//! Overridden from GamepadDevice
	//! Returns the locally-known axis name for a given code
	const char *get_axis_name(u32 code) override;

	//! Overridden from AndroidGamepadDevice
	//! Called just before destruction in order to do cleanup
	void close(JNIEnv *env) override;

	//! Determines if a VID/PID is a DreamPicoPort gamepad
	//! @param[in] vendorId Vendor ID (16-bit value)
	//! @param[in] productId Product ID (16-bit value)
	//! @return true iff the given VID/PID is a DreamPicoPort
	static bool identify(int vendorId, int productId);

	//! Overridden from GamepadDevice
	//! @return the sort ID in order to ensure the proper display order of DreamPicoPort devices
	inline const std::string& sort_id() override
	{
		return !_sort_id.empty() ? _sort_id : DreamLinkAndroidGamepad::sort_id();
	}

	//! Overridden from GamepadDevice
	//! Checks for gamepad-changed events
	bool gamepad_btn_input(u32 code, bool pressed) override;

	//! Overridden from DreamLinkAndroidGamepad
	//! @return true if this device couldn't fully connect because it's waiting on permission from the user
	inline bool isAwaitingPermission() const override
	{
		return (dpp == nullptr);
	}

protected:
	//! Overridden from DreamLinkAndroidGamepad
	void setCustomMapping(const std::shared_ptr<InputMapping>& mapping) override;

private:
	//! The DreamPicoPort device handle associated with this gamepad (nullptr when waiting for permission)
	std::shared_ptr<class AndroidDreamPicoPort> dpp;
	//! The raw name provided from Android
	const std::string android_name;
	//! ID used for sorting on the UI
	std::string _sort_id;
};