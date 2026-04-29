/*
	Copyright 2024 flyinghead

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

// This file contains abstraction layer for access to different kinds of remote peripherals.
// This includes both real Dreamcast controllers, VMUs, jump packs etc. but also emulated VMUs.

#include "types.h"
#include "emulator.h"
#include "../sdl_gamepad.h"
#include "sdldreamlink.h"

#include <functional>
#include <memory>
#include <array>

class DreamLinkGamepad : public SDLGamepad
{
public:
	const char* dreamLinkStatus();
	void set_maple_port(int port) override;
	void registered() override;
	static bool isDreamcastController(int deviceIndex);
	void resetMappingToDefault(bool arcade, bool gamepad) override;
	void close() override;

protected:
	DreamLinkGamepad(std::shared_ptr<SDLDreamLink> dreamlink, int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick);
	std::shared_ptr<InputMapping> getDefaultMapping() override;
	void setBaseDefaultMapping(const std::shared_ptr<InputMapping>& mapping) const;
	virtual void setCustomMapping(const std::shared_ptr<InputMapping>& mapping) {}

	std::shared_ptr<SDLDreamLink> dreamlink;
	std::string device_guid;
};

std::shared_ptr<DreamLinkGamepad> createDreamLinkGamepad(int maple_port, int joystick_idx, SDL_Joystick* sdl_joystick);
#endif // USE_DREAMLINK_DEVICES
