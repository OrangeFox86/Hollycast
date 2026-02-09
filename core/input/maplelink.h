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
#include "log/Log.h"
#include <array>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

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
	std::shared_ptr<maple_device> createMapleDevice();

	//! The associated dreamlink
	const DreamLink::Ptr dreamlink;
	//! The linked bus
	const u32 bus;
	//! The linked port
	const u32 port;
};

/*
 * Maple Link Device Serialization/Deserialization
 * - On serialize, the device must make a best guess of what device is attached and serialize in the same way that
 *   virtual device would serialize as
 * - On deserialize, the device will decide if it can accept incoming data by implementing deserializingFor() then the
 *   subsequent deserialize will handle the incoming data
*/

//! Virtual class which all maple link devices must inheirt from
struct MapleLinkDevice
{
	//! @return true iff there is a current link to external storage
	virtual bool usingExternalStorage() const = 0;

	//! This is called by the deserialization process to relay what deserialization virtual device type
	//! @param[in] type The type that should be assumed for subsequent deserialize
	//! @return true iff this MapleLink is setup to accept this type
	virtual bool deserializingFor(MapleDeviceType type) = 0;
};

//! Basic maple link VMU device which relays only screen and timer data to the MapleLink
struct MapleLinkVmu : maple_sega_vmu, MapleLinkDevice
{
	u32 dma(u32 cmd) override;
	u32 dma(std::optional<MapleLink>& link, u32 cmd);
	inline bool usingExternalStorage() const override
	{
		return false;
	}
	inline bool deserializingFor(MapleDeviceType type) override
	{
		return (type == MDT_SegaVMU);
	}

	// Can safely use serialization from maple_sega_vmu
	using maple_sega_vmu::serialize;
	using maple_sega_vmu::deserialize;
};

//! Basic maple link VMU device which relays only screen and timer data to the MapleLink
struct MapleLinkPuruPuru : maple_sega_purupuru, MapleLinkDevice
{
	u32 dma(u32 cmd) override;
	u32 dma(std::optional<MapleLink>& link, u32 cmd);
	inline bool usingExternalStorage() const override
	{
		return false;
	}
	inline bool deserializingFor(MapleDeviceType type) override
	{
		return (type == MDT_PurupuruPack);
	}

	// Can safely use serialization from maple_sega_purupuru
	using maple_sega_purupuru::serialize;
	using maple_sega_purupuru::deserialize;
};

//! Stub class which may be used as a placeholder device when no function is needed. This is used to reserve a space
//! in maple devices array so that a lower priority DreamLink can't install itself there.
struct MapleLinkStub : maple_base, MapleLinkDevice
{
	inline MapleDeviceType get_device_type() override { return MapleDeviceType::MDT_None; }
	inline u32 dma(u32 cmd) override { return MDRS_JVSNone; }
	inline bool linkStatus() override { return false; }
	inline bool usingExternalStorage() const { return false; }
	inline bool deserializingFor(MapleDeviceType type) override { return false; }
	inline void serialize(Serializer& ser) const override
	{
		// Should never reach here because get_device_type() returns MDT_None
		ERROR_LOG(INPUT, "MapleLinkStub received unexpected Serializer");
	}
	inline void deserialize(Deserializer& deser) override
	{
		// Should never reach here because deserializingFor() returns false
		ERROR_LOG(INPUT, "MapleLinkStub received unexpected Deserializer");
	}
};
