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

#include "hw/maple/maple_devs.h"
#include "emulator.h"

#include <array>
#include <list>
#include <initializer_list>
#include <mutex>
#include <atomic>
#include <unordered_map>

// Abstract base class for communication with physical controllers
class DreamLink
{
public:
	using Ptr = std::shared_ptr<DreamLink>;

    //! Constructor (default)
    DreamLink() = default;
    //! Destructor (virtual, default)
	virtual ~DreamLink() = default;

    //! Copy constructor (deleted)
    DreamLink(const DreamLink&) = delete;
    //! Assignment operator (deleted)
    DreamLink& operator=(const DreamLink&) = delete;

	//! Sends a message to the controller, not expecting a response
    //! @return true iff send was at least scheduled (no guarantee of completion)
	virtual bool send(const MapleMsg& msg) = 0;
	//! Sends a message to the controller and waits for a response
    //! @return true iff both send and receive fully completed
	virtual bool sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg) = 0;
	//! @return true iff VMU reads and writes should be sent to the device
	virtual bool storageEnabled() = 0;
	//! @return true iff a game has been started
	virtual bool isGameStarted() const = 0;
	//! Attempt connection to the link
	virtual void connect() = 0;
	//! Disconnect from the link
	virtual void disconnect() = 0;
	//! @return true iff the link is operational
	virtual bool isConnected() = 0;
	//! Changes the selected maple port is changed by the user
	virtual void changeBus(int newBus) = 0;

	//! Create the maple device needed to interface this device to the emulator
	//! @param[in] type The user-selected device type
	//! @param[in] bus The bus used for this device [0,3]
	//! @param[in] port The port for this device [0,5]
	virtual std::shared_ptr<maple_device> createMapleDevice(MapleDeviceType type, int bus, int port) = 0;

	//! Check if a given bus is valid
	//! @param[in] bus The dreamcast bus index to test
	//! @return true iff bus is a valid physical bus
	static inline bool isValidBus(int bus) {
		return (bus >= 0 && bus < MAPLE_PORTS);
	}

    //! Check if a given port is valid
    //! @param[in] port The logical port number to check
    //! @return true iff port is a valid logical maple port
    static inline bool isValidPort(int port) {
        return (port >= 0 && port < MAPLE_DEVS_PER_PORT);
    }
};

//! The class a DreamLink device should inherit from
class BaseDreamLink : public DreamLink, public std::enable_shared_from_this<BaseDreamLink>
{
public:
	using Ptr = std::shared_ptr<BaseDreamLink>;

protected:
    //! Default constructor (deleted)
    BaseDreamLink() = delete;
    //! Constructor
    //! @param[in] storageSupported True if this DreamLink supports physical VMU memory
	BaseDreamLink(bool storageSupported);

public:
    //! Destructor (virtual)
	virtual ~BaseDreamLink();
	//! @return true iff storage is supported AND currently enabled for this DreamLink
	bool storageEnabled() override;
	//! @return true iff a game has been started
	bool isGameStarted() const;

protected:
    //! Disable VMU storage for this link
	void disableStorage();
	//! Called when a game has started
	virtual void onGameStarted();
	//! Called when a game has terminated
	//! When called, do teardown stuff (vmu screen reset is handled by maple_sega_vmu)
	virtual inline void onGameTermination() {}

    //! Registers this link into registries.
    //! The registry in DreamLink ensures only 1 DreamLink per bus is active. The global registry links logical
	//! bus/ports to this DreamLink. If this DreamLink was already registered, the previous registration will first be
	//! deleted.
    //! @param[in] bus The bus that this DreamLink belongs to (a DreamLink may only belong to a single bus)
    //! @param[in] ports The port mask representing the ports on the bus that this DreamLink supports
    void registerLink(int bus, u32 ports);

    //! Unregisters this DreamLink from all registries
    void unregisterLink();

	//! Overridden from DreamLink
	//! This is the default implementation which may be overridden by child
	std::shared_ptr<maple_device> createMapleDevice(MapleDeviceType type, int bus, int port) override;

private:
    //! Removes link from reistry without locking on the mutex
    //! @param[in] newBus When non-negative, this is the new bus that will be subsequently set for this link
    void removeLinkFromRegistry(int newBus = -1);

    //! Esablish this link in MapleLinkRegistry
    //! @param[in] bus The bus that this DreamLink belongs to (a DreamLink may only belong to a single bus)
    //! @param[in] ports The port mask representing the ports on the bus that this DreamLink supports
    void establishInMapleLinkRegistry(int bus, u32 ports);

    //! Central event handler
    //! @param[in] event The incoming event
    //! @param[in] p Context pointer (ignored)
	static void eventHandler(Event event, void *p);

public:
    //! Mask to use for all ports
    static constexpr const u32 ALL_PORTS_MASK = (1 << MAPLE_DEVS_PER_PORT) - 1;

protected:
    //! Determines whether or not storage is supported by this DreamLink
    const bool storageSupported;

private:
    //! Runtime storage enable flag
	bool vmuStorage = false;

    //! Currently linked bus
    int linkedBus;
    //! All available ports
    u32 linkedPorts;
    //! All currently connected ports
    u32 connectedPorts;

    //! Initialized to false and set to true on the creation of the first BaseDreamLink
    static std::atomic<bool> EventsRegistered;
    //! Flag which tracks game start/termination events
    static std::atomic<bool> GameStarted;

    //! Mutex serializing access to Registry
    static std::recursive_mutex Mutex;
    //! Registry of BaseDreamLink devices [bus index][priority] (front takes precedence)
    static std::array<std::list<Ptr>, MAPLE_PORTS> Registry;

};

//! Basic dream link VMU device which relays only screen and timer data to the DreamLink device
struct DreamLinkVmu : maple_sega_vmu
{
	u32 dma(u32 cmd) override;
};

//! Basic dream link VMU device which relays only screen and timer data to the DreamLink device
struct DreamLinkPuruPuru : maple_sega_purupuru
{
	u32 dma(u32 cmd) override;
};
