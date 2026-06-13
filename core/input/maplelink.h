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
#include <chrono>

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
	bool storageEnabled() const;

	//! True if the link is operational
	bool isConnected();

	//! @return the function code for this MapleLink
	u32 getFunctionCodesMask() const;

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

//! Virtual class which describes additional interfaces for a MapleLinkDevice
struct MapleLinkDevice
{
	//! This is called by the deserialization process to relay what deserialization virtual device type
	//! @param[in] type The type that should be assumed for subsequent deserialize
	//! @return true iff this MapleLink is setup to accept this type
	virtual bool deserializingFor(MapleDeviceType type) = 0;

protected:
	//! Deserializes VMU data for a MapleLinkDevice
	//! @param[in] deser The deserializer to pull data from
	//! @param[in] link The MapleLink to communicate with during deserialization
	//! @param[in] dev The device that the DreamLink created
	//! @param[in] vmu The vmu device used for deserialization (may or may not be the same as dev)
	//! @param[in] usingExternalMemory When true, do checks that external memory matches deserialized data
	static void deserializeVmu(
		Deserializer& deser,
		MapleLink& link,
		maple_base& dev,
		maple_sega_vmu& vmu,
		bool usingExternalMemory
	);

	//! Send a read request to the external VMU memory
	//! @param[in] link The MapleLink to communicate with
	//! @param[in] dest Destination address
	//! @param[in] origin Origin address
	//! @param[in] block Block index
	//! @return std::nullopt if execution failed
	//! @return the received response from the external VMU
	static std::optional<MapleMsg> sendRead(MapleLink& link, u8 dest, u8 origin, u8 block);

	//! Mirrors the LCD data from VMU data to a MapleLink device
	//! @param[in] link The MapleLink to send LCD data to
	//! @param[in] dev The device that the DreamLink created
	//! @param[in] vmu The VMU containing LCD data (may or may not be the same as dev)
	static void mirrorLcd(MapleLink& link, maple_base& dev, maple_sega_vmu& vmu);

	//! Relays a maple message to the currently active physical DreamLink device on the given bus and port
	//! @param[in] bus The target DreamLink bus
	//! @param[in] port The target DreamLink port
	//! @param[in] msg The message to send
	static void relayPhysicalMapleLink(int bus, int port, const MapleMsg& msg);
};

//! Base class all maple link devices must inherit from
//! @tparam MapleDeviceBase The maple_base class to inherit functionality from
template <
	typename MapleDeviceBase,
	typename std::enable_if_t<std::is_base_of_v<maple_base, MapleDeviceBase>>* = nullptr
>
struct MapleLinkDeviceBase: public MapleDeviceBase, public MapleLinkDevice
{
	//! The linked device
	MapleLink link;

	//! Default constructor (deleted)
	MapleLinkDeviceBase() = delete;

	//! Constructor
	//! @param[in] link The link to set
	explicit inline MapleLinkDeviceBase(const MapleLink& link) : link(link) {}

	//! @return true iff this device is currently present
	bool linkStatus() override
	{
		return (MapleDeviceBase::linkStatus() && link.isConnected() && (link.getFunctionCodesMask() != 0));
	}

protected:
	//! Deserializes VMU data for a MapleLinkDevice, defined when the MapleDeviceBase is a maple_sega_vmu
	//! @param deser The deserializer to pull data from
	//! @param usingExternalMemory When true, do checks that external memory matches deserialized data
	template<
		typename U = MapleDeviceBase,
		typename std::enable_if<std::is_same<U, maple_sega_vmu>::value>::type* = nullptr
	>
	inline void deserializeVmu(Deserializer& deser, bool usingExternalMemory)
	{
		MapleLinkDevice::deserializeVmu(deser, link, *this, *this, usingExternalMemory);
	}

	//! Deserializes VMU data for a MapleLinkDevice
	//! @param deser The deserializer to pull data from
	//! @param vmu The vmu device used for deserialization
	//! @param usingExternalMemory When true, do checks that external memory matches deserialized data
	inline void deserializeVmu(Deserializer& deser, maple_sega_vmu& vmu, bool usingExternalMemory)
	{
		MapleLinkDevice::deserializeVmu(deser, link, *this, vmu, usingExternalMemory);
	}

	//! Mirrors the LCD data from VMU data to a MapleLink device, defined when the MapleDeviceBase is a maple_sega_vmu
	template<
		typename U = MapleDeviceBase,
		typename std::enable_if<std::is_same<U, maple_sega_vmu>::value>::type* = nullptr
	>
	inline void mirrorLcd()
	{
		MapleLinkDevice::mirrorLcd(link, *this, *this);
	}

	//! Mirrors the LCD data from VMU data to a MapleLink device
	//! @param vmu The VMU containing LCD data
	inline void mirrorLcd(maple_sega_vmu& vmu)
	{
		MapleLinkDevice::mirrorLcd(link, *this, vmu);
	}

	//! This is called by a virtual device when a feedback message relay is requested
	inline void relayMapleLink() override
	{
		if (inMsg) {
			MapleLinkDevice::relayPhysicalMapleLink(bus_id, bus_port, *inMsg);
		}
	}
};

//! Basic maple link VMU device which relays only screen and timer data to the MapleLink
struct MapleLinkVmu : MapleLinkDeviceBase<maple_sega_vmu>
{
	MapleLinkVmu() = delete;
	MapleLinkVmu(const MapleLink& link);

	void OnSetup() override;
	bool fullSave() override;
	u32 dma(u32 cmd) override;
	inline bool deserializingFor(MapleDeviceType type) override
	{
		return (type == MDT_SegaVMU);
	}

	// Can safely use maple_sega_vmu::serialize()
	using maple_sega_vmu::serialize;
	//! Performs deserialization and writes deserialized LCD
	void deserialize(Deserializer& deser) override;

	//! @return true iff this device is currently present
	bool linkStatus() override
	{
		if (!MapleLinkDeviceBase<maple_sega_vmu>::linkStatus())
			return false;

		if (link.storageEnabled())
			return link.isConnected();
		else
			return true; // local storage means this must always remain "linked"
	}

	//! Send a read request to the external VMU memory
	//! @param[in] dest Destination address
	//! @param[in] origin Origin address
	//! @param[in] block Block index
	//! @return std::nullopt if execution failed
	//! @return the received response from the external VMU
	inline std::optional<MapleMsg> sendRead(u8 dest, u8 origin, u8 block)
	{
		return MapleLinkDevice::sendRead(link, dest, origin, block);
	}
};

//! Basic maple link VMU device which relays only screen and timer data to the MapleLink
struct MapleLinkPuruPuru : MapleLinkDeviceBase<maple_sega_purupuru>
{
	MapleLinkPuruPuru() = delete;
	MapleLinkPuruPuru(const MapleLink& link);

	u32 dma(u32 cmd) override;
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
struct MapleLinkStub : MapleLinkDeviceBase<maple_base>
{
	inline MapleLinkStub() : MapleLinkDeviceBase<maple_base>(MapleLink(nullptr, 0, 0)) {}

	inline MapleDeviceType get_device_type() override { return MapleDeviceType::MDT_None; }
	inline u32 dma(u32 cmd) override { return MDRS_JVSNone; }
	inline bool linkStatus() override { return false; }
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
