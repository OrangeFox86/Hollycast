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

#include "types.h"
#include "emulator.h"
#include "android_gamepad.h"
#include "input/dreamlink/gamepad_dreamlink.h"

#include <functional>
#include <memory>
#include <array>

//! Android DreamLink gamepad base class
class DreamLinkAndroidGamepad : public AndroidGamepadDevice
{
protected:
	//! Constructor
	//! @param[in] dreamlink The DreamLink associated with this gamepad
	//! @param[in] maple_port The requested maple port index to use
	//! @param[in] joystickData All joystick data associated with the InputDevice
	DreamLinkAndroidGamepad(
		std::shared_ptr<GamepadDreamLink> dreamlink,
		int maple_port,
		const AndroidGamepadDevice::AndroidJoystickData& joystickData
	);

public:
	//! Destructor
	virtual ~DreamLinkAndroidGamepad() = default;

	//! Overridden from GamepadDevice
	//! @return "Connection Failed", "Connected", or "Disconnected"
	const char* status() override;

	//! Overridden from GamepadDevice
	void set_maple_port(int port) override;

	//! Overridden from GamepadDevice
	void registered() override;

	//! Determines if the given VID/PID is a DreamLink gamepad device
	//! @param[in] vid Vendor ID
	//! @param[in] pid Product ID
	//! @return true iff the given VID/PID is a DreamLink gamepad device
	static bool isDreamLinkGamepad(int vid, int pid);

	//! Determines if permission needs to be requested for a UsbDevice with the given VID/PID
	//! @param[in] vid Vendor ID
	//! @param[in] pid Product ID
	//! @return true iff permission needs to be requested for a UsbDevice with the given VID/PID
	static bool isPermissionRequired(int vid, int pid);

	//! Overridden from AndroidGamepadDevice
	//! Sets DreamLink-specific default mapping
	void resetMappingToDefault(bool arcade, bool gamepad) override;

	//! Overridden from AndroidGamepadDevice
	//! Called just before destruction in order to do cleanup
	void close(JNIEnv *env) override;

	//! @return true iff this device is waiting for permission from the user
	virtual bool isAwaitingPermission() const = 0;

protected:
	//! Overridden from AndroidGamepadDevice
	//! @return DreamLink-specific default mapping
	std::shared_ptr<InputMapping> getDefaultMapping() override;

	//! Set the base default mapping for a DreamLink gamepad device
	//! @param[in,out] mapping The mapping to modify
	void setBaseDefaultMapping(const std::shared_ptr<InputMapping>& mapping) const;

	//! This may be overridden by the child in order to set default mapping specific to the child
	//! @param[in,out] mapping The mapping to modify
	virtual void setCustomMapping(const std::shared_ptr<InputMapping>& mapping) {}

	//! The associated DreamLink device
	std::shared_ptr<GamepadDreamLink> dreamlink;
};

//! Create DreamLink gamepad for a given InputDevice associated with a DreamLink gamepad
//! @param[in] env Local Java environment
//! @param[in] usbManager A UsbManager object created from the current app context
//! @param[in] maple_port The requested maple port index to use
//! @param[in] joystickData All joystick data associated with the InputDevice
//! @return nullptr if VID/PID was not associated with a valid DreamLink device
//! @return a new DreamLinkAndroidGamepad otherwise
std::shared_ptr<DreamLinkAndroidGamepad> createDreamLinkAndroidGamepad(
	JNIEnv *env,
	jobject usbManager,
	int maple_port,
	const AndroidGamepadDevice::AndroidJoystickData& joystickData
);
