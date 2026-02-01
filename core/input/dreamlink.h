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

#include "maplelink.h"
#include "hw/maple/maple_devs.h"

// Abstract base class for communication with physical controllers
class DreamLink : public BaseMapleLink
{
public:
	//! Number of physical dreamcast ports (buses)
	static constexpr int NUM_PORTS = MAPLE_PORTS;

	//! Check if a given port is valid
	//! @param[in] port The dreamcast port index to test
	//! @return true iff port is a valid physical port
	static bool isValidPort(int port) {
		return (port >= 0 && port < NUM_PORTS);
	}

	//! Changes the selected maple port is changed by the user
	virtual void changeBus(int newBus) = 0;

	//! Returns true if connected to the hardware controller
	virtual bool isConnected() = 0;

	//! Attempt connection to the hardware controller
	virtual void connect() = 0;

	//! Disconnect from the hardware controller
	virtual void disconnect() = 0;

protected:
	DreamLink(bool storageSupported = true)
		: BaseMapleLink(storageSupported)
	{}
};
