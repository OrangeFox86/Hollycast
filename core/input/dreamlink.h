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
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <memory>
#include <functional>

// Abstract base class for communication with physical controllers
class DreamLink
{
	// MapleLink is tightly coupled with this class and may access createMapleDevice()
	friend class MapleLink;

public:
	using Ptr = std::shared_ptr<DreamLink>;
	using WPtr = std::weak_ptr<DreamLink>;

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
	virtual bool isGameRunning() const = 0;
	//! Attempt connection to the link
	virtual void connect() = 0;
	//! Disconnect from the link
	virtual void disconnect() = 0;
	//! @return true iff the link is operational
	virtual bool isConnected() = 0;
	//! This should be overridden by the child in order to report fatal errors preventing all operations
	//! @return nullptr if no issue exists
	//! @return pointer to the issue description string; no operation is expected to succeed in this case
	virtual const char* getIssueDescription() const = 0;
	//! Do termination cleanup
	//! @post the object may be in an invalid state and is no longer intended for use
	virtual void term() = 0;
	//! @return the display name of this DreamLink
	virtual const char* getName() const = 0;

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

	//! Executes something for each port in a port mask
	//! @param[in] portsMask Mask of ports (up to ALL_PORTS_MASK)
	//! @param[in] fn The function to execute for each port in the mask
	//! @return number of times fn was called
	static inline std::size_t forEachPort(u32 portsMask, std::function<void(int port)> fn) {
		std::size_t num = 0;

		for (int port = 0; port < MAPLE_DEVS_PER_PORT; ++port)
		{
			if (((1 << port) & portsMask) == 0)
			{
				continue;
			}

			fn(port);
			++num;
		}

		return num;
	}

    //! Mask to use for all ports
    static constexpr const u32 ALL_PORTS_MASK = (1 << MAPLE_DEVS_PER_PORT) - 1;

protected:
	//! Create the maple device needed to interface this device to the emulator
	//! @param[in] bus The bus used for this device [0,3]
	//! @param[in] port The port for this device [0,5]
	virtual std::shared_ptr<maple_device> createMapleDevice(int bus, int port) = 0;
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
	//! @return true iff storage is supported AND currently enabled for this DreamLink
	bool storageEnabled() override;
	//! @return true iff a game has been started
	bool isGameRunning() const;
	//! Child may override this if it needs to report fatal errors
	const char* getIssueDescription() const override;
	//! Do termination cleanup
	//! @post the object may be in an invalid state and is no longer intended for use
	void term() override;

protected:
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
    //! @param[in] portsMask The port mask representing the ports on the bus that this DreamLink supports
    void registerLink(int bus, u32 portsMask);

    //! Unregisters this DreamLink from all registries
	//! @param[in] isTerminal Set to true when unregistration needs to be done due to terminal event
    void unregisterLink(bool isTerminal = false);

	//! Overridden from DreamLink
	//! This is the default implementation which may be overridden by child
	std::shared_ptr<maple_device> createMapleDevice(int bus, int port) override;

protected:
    //! Determines whether or not storage is supported by this DreamLink
    const bool storageSupported;

private:
    //! Currently linked bus
    int linkedBus = -1;
    //! All available ports
    u32 linkedPortsMask = 0;
    //! All currently connected ports
    u32 connectedPortsMask = 0;

	//! Singleton class which is a prioritized Registry of BaseDreamLink devices.
	//! This registry keeps track of which devices are connected by priority and handles game events for all devices.
	class PrioritizedRegistry
	{
	private:
		//! Default constructor
		PrioritizedRegistry();
		//! Destructor
		~PrioritizedRegistry();

	public:
		//! @return the singleton instance
		static PrioritizedRegistry& Get();

		//! Esablish this link in MapleLinkRegistry
		//! @param[in] bus The bus that this DreamLink belongs to (a DreamLink may only belong to a single bus)
		//! @param[in] portsMask The port mask representing the ports on the bus that this DreamLink supports
		void registerLink(const BaseDreamLink::Ptr& dreamlink, int bus, u32 portsMask);

		//! Removes link from reistry without locking on the mutex
		//! @param[in] newBus When non-negative, this is the new bus that will be subsequently set for this link
		//! @param[in] isTerminal Set to true when unregistration needs to be done due to terminal event
		void unregisterLink(BaseDreamLink* dreamlink, bool isTerminal = false);

		//! @return true iff game is currently running
		bool isGameRunning() const;

	private:
	    //! Central event handler
		//! @param[in] event The incoming event
		void eventHandler(Event event);

		//! Static event handler
		//! @param[in] event The incoming event
		//! @param[in] p Pointer to a PrioritizedRegistry
		static void EventHandler(Event event, void *p);

		//! Esablish this link in MapleLinkRegistry
		//! @param[in] dreamlink Smart pointer to the dreamlink to add
		//! @param[in] bus The bus that this DreamLink belongs to (a DreamLink may only belong to a single bus)
		//! @param[in] portsMask The port mask representing the ports on the bus that this DreamLink supports
		void establishInMapleLinkRegistry(const BaseDreamLink::Ptr& dreamlink, int bus, u32 portsMask);

		//! Removes link from registry without locking on the mutex
		//! @param[in] dreamlink Pointer to the dreamlink to remove
		//! @param[in] newBus When non-negative, this is the new bus that will be subsequently set for this link
		void removeLinkFromRegistry(const BaseDreamLink* dreamlink, int newBus = -1);

	private:
		//! Flag which tracks game start/termination events
		std::atomic<bool> mIsGameRunning;

		//! Mutex serializing access to Registry
		std::recursive_mutex mMutex;
		//! Registry of BaseDreamLink devices [bus index][priority] (front takes precedence)
		std::array<std::list<BaseDreamLink::Ptr>, MAPLE_PORTS> mRegistry;
	};
};
