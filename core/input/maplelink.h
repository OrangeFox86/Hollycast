/*
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
#include "dreamlink.h"
#include "types.h"
#include "hw/maple/maple_devs.h"
#include "emulator.h"
#include <array>
#include <list>
#include <memory>
#include <mutex>

//! A MapleLink puts bus/port context onto a DreamLink and allows for creation of a maple_device
class MapleLink
{
public:
	//! Default constructor (deleted)
	MapleLink() = delete;

	//! Constructor
	//! @param[in] dreamlink The dreamlink pointer to copy from
	//! @param[in] bus Dreamcast bus index [0,3]
	//! @param[in] port Peripheral port index [0,5]
	MapleLink(const DreamLink::Ptr& dreamlink, u32 bus, u32 port);

	//! Constructor
	//! @param[in] dreamlink The dreamlink pointer to move from
	//! @param[in] bus Dreamcast bus index [0,3]
	//! @param[in] port Peripheral port index [0,5]
	MapleLink(DreamLink::Ptr&& dreamlink, u32 bus, u32 port);

	//! Destructor (virtual, default)
	virtual ~MapleLink() = default;

	//! Sends a message to the controller, not expecting a response
	bool send(const MapleMsg& msg);

	//! Sends a message to the controller and waits for a response
	bool sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg);

	//! True if VMU reads and writes should be sent to the device
	bool storageEnabled();

	//! True if the link is operational
	bool isConnected();

	//! Create the maple device needed to interface this device to the emulator
	//! @param[in] type The user-selected device type
	//! @param[in] bus The bus used for this device [0,3]
	//! @param[in] port The port for this device [0,4]
	std::shared_ptr<maple_device> createMapleDevice(MapleDeviceType type);

	//! The associated dreamlink
	const const DreamLink::Ptr dreamlink;
	//! The linked bus
	const u32 bus;
	//! The linked port
	const u32 port;
};

//! Basic maple link VMU device which relays only screen and timer data to the MapleLink
struct MapleLinkVmu : maple_sega_vmu
{
	u32 dma(u32 cmd) override;
};

//! Basic maple link VMU device which relays only screen and timer data to the MapleLink
struct MapleLinkPuruPuru : maple_sega_purupuru
{
	u32 dma(u32 cmd) override;
};
