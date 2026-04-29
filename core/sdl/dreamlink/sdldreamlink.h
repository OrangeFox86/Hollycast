/*
	Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
	https://github.com/OrangeFox86/Hollycast

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

#include <input/dreamlink.h>

class SDLDreamLink : public BaseDreamLink
{
public:
    //! Constructor
    //! @param[in] storageSupported True if this DreamLink supports physical VMU memory
    inline SDLDreamLink(bool storageSupported) : BaseDreamLink(storageSupported)
    {}

    //! Destructor (virtual, default)
    virtual ~SDLDreamLink() = default;

	//! Changes the selected maple port is changed by the user
	virtual void changeBus(int newBus) = 0;

	//! Called once the gamepad is registered - the dreamlink should register itself
	virtual void registered() = 0;

	//! Registers this link into registries.
    //! The registry in DreamLink ensures only 1 DreamLink per bus is active. The global registry links logical
	//! bus/ports to this DreamLink. If this DreamLink was already registered, the previous registration will first be
	//! deleted.
    //! @param[in] bus The bus that this DreamLink belongs to (a DreamLink may only belong to a single bus)
    //! @param[in] portsMask The port mask representing the ports on the bus that this DreamLink supports
    inline void registerLink(int bus, u32 portsMask)
	{
		// All SDL hardware DreamLink devices take lower priority over software DreamLink devices
		BaseDreamLink::registerLink(bus, portsMask, LinkPriority::LOW);
	}
};
