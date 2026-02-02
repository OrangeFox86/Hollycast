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

#include "dreamlink.h"
#include "maplelink.h"
#include "maplelinkregistry.h"
#include "cfg/option.h"
#include "hw/maple/maple_if.h"
#include "oslib/oslib.h"

std::recursive_mutex BaseDreamLink::Mutex;
std::array<std::list<BaseDreamLink::Ptr>, MAPLE_PORTS> BaseDreamLink::Registry;
std::atomic<bool> BaseDreamLink::EventsRegistered = false;
std::atomic<bool> BaseDreamLink::GameStarted = false;

BaseDreamLink::BaseDreamLink(bool storageSupported) :
	storageSupported(storageSupported),
	vmuStorage(storageSupported),
	linkedBus(-1),
	linkedPorts(0),
	connectedPorts(0)
{
	if (isGameStarted()) {
		vmuStorage = vmuStorage && config::UsePhysicalVmuMemory;
	}

	// Register events on first BaseDreamLink instance
	const bool eventsRegistered = EventsRegistered.exchange(true);
	if (!eventsRegistered) {
		EventManager::listen(Event::LoadState, eventHandler, nullptr);
		EventManager::listen(Event::Start, eventHandler, nullptr);
		EventManager::listen(Event::Terminate, eventHandler, nullptr);
	}
}

BaseDreamLink::~BaseDreamLink()
{
	// This link should no longer be registered anywhere
	std::lock_guard<std::recursive_mutex> lock(BaseDreamLink::Mutex);

	// Remove everything previously established for this controller
	removeLinkFromRegistry();
}

bool BaseDreamLink::storageEnabled()
{
	if (!isConnected())
		return false;
	if (!isGameStarted())
		return storageSupported && config::UsePhysicalVmuMemory;
	else
		return vmuStorage;
}


void BaseDreamLink::disableStorage()
{
	if (!vmuStorage)
		return;
	WARN_LOG(INPUT, "State loaded but VMU has storage enabled");
	vmuStorage = false;
	if (isGameStarted())
	{
		// TODO: Only DreamPotato would be affected by this, so maybe there just needs to be a tweak there
		emu.run([bus=linkedBus, ports=connectedPorts]() {
			for (int port = 0; port < MAPLE_DEVS_PER_PORT; ++port)
			{
				if (DreamLink::isValidBus(bus) && (ports & (1 << port)))
					maple_ReconnectDevice(bus, port);
			}
		});

		os_notify("ATTENTION: State loaded with physical VMU memory enabled", 6000,
				"Physical memory has detached from local state");
	}
}

bool BaseDreamLink::isGameStarted() const
{
	return BaseDreamLink::GameStarted;
}

void BaseDreamLink::term()
{
	unregisterLink();
}

void BaseDreamLink::onGameStarted()
{
	vmuStorage = storageSupported && config::UsePhysicalVmuMemory && isConnected();
}

void BaseDreamLink::registerLink(int bus, u32 ports)
{
	if (linkedBus == bus && connectedPorts == ports)
		return;

	std::lock_guard<std::recursive_mutex> lock(BaseDreamLink::Mutex);

	// Ensure this link is not currently established
	removeLinkFromRegistry(bus);

	if (isValidBus(bus))
	{
		linkedBus = bus;
		linkedPorts = ports;
		connectedPorts = ports;

		std::list<BaseDreamLink::Ptr>& priorities = BaseDreamLink::Registry[bus];

		if (!priorities.empty())
		{
			// Remove connected port flags since this link now takes precedence
			for (BaseDreamLink::Ptr& link : priorities)
			{
				const u32 prev = link->connectedPorts;
				link->connectedPorts = link->connectedPorts & ~ports;
				if (link->connectedPorts == 0)
				{
					if (prev != link->connectedPorts)
					{
						// No longer connected to anything
						link->disconnect();
					}
				}
			}
		}

		// Establish local registry link
		priorities.push_front(shared_from_this());

		// Connect now if not already
		if (!isConnected())
			connect();

		establishInMapleLinkRegistry(bus, ports);
	}
	else
	{
		// Not a valid bus, so this should not be connected
		linkedBus = -1;
		disconnect();
	}
}

void BaseDreamLink::unregisterLink()
{
	if (!isValidBus(linkedBus))
		return;

	std::lock_guard<std::recursive_mutex> lock(BaseDreamLink::Mutex);

	linkedBus = -1;

	disconnect();

	// Remove everything previously established for this controller
	removeLinkFromRegistry();
}

std::shared_ptr<maple_device> BaseDreamLink::createMapleDevice(MapleDeviceType type, int bus, int port)
{
	// By default, make a VMU and PuruPuru device link to hardware
	switch (type) {
		case MapleDeviceType::MDT_SegaVMU: return std::make_shared<MapleLinkVmu>();
		case MapleDeviceType::MDT_PurupuruPack: return std::make_shared<MapleLinkPuruPuru>();
		default: return maple_Create(type);
	}
}

void BaseDreamLink::removeLinkFromRegistry(int newBus)
{
	// Remove from MapleLinkRegistry
	MapleLinkRegistry::Get().unregisterLink(this);

	for (int bus = 0; bus < BaseDreamLink::Registry.size(); ++bus)
	{
		std::list<Ptr>& priorities = BaseDreamLink::Registry[bus];
		std::list<Ptr>::iterator iter = priorities.begin();
		while (iter != priorities.end())
		{
			if (iter->get() == this)
			{
				iter = priorities.erase(iter);

				if (bus != newBus && iter != priorities.end())
				{
					// Establish "new" priorities into MapleLinkRegistry
					u32 availablePorts = ALL_PORTS_MASK;
					for (std::list<Ptr>::iterator innerIter = iter; innerIter != priorities.end(); ++innerIter)
					{
						const Ptr& link = *innerIter;
						const u32 ports = availablePorts & link->linkedPorts;
						if (ports != 0)
						{
							link->establishInMapleLinkRegistry(bus, ports);
							availablePorts = availablePorts & ~ports;
						}
					}
				}
			}
			else
			{
				++iter;
			}
		}
	}
}

void BaseDreamLink::establishInMapleLinkRegistry(int bus, u32 ports)
{
	bool added = false;
	for (u32 port = 0; port < MAPLE_DEVS_PER_PORT; ++port)
	{
		const u32 portMask = (1 << port);
		if ((portMask & ports) != 0)
		{
			connectedPorts |= portMask;
			MapleLinkRegistry::Get().registerLink(shared_from_this(), bus, port);
			added = true;
		}
	}

	if (added && !isConnected())
		connect();
}

void BaseDreamLink::eventHandler(Event event, void *p)
{
	// Set GameStarted flag
	switch (event)
	{
	case Event::Start:
		BaseDreamLink::GameStarted.store(true);
		break;
	case Event::Terminate:
		BaseDreamLink::GameStarted.store(false);
		break;
	default:
		break;
	}

	// Perform events for each link
	std::lock_guard<std::recursive_mutex> lock(BaseDreamLink::Mutex);
	for (std::list<Ptr>& priorities : BaseDreamLink::Registry)
	{
		if (!priorities.empty())
		{
			// Only perform events for the priority device
			const Ptr& ptr = priorities.front();
			switch (event)
			{
			case Event::Start:
				ptr->onGameStarted();
				break;
			case Event::Terminate:
				ptr->onGameTermination();
				break;
			case Event::LoadState:
				ptr->disableStorage();
				break;
			default:
				break;
			}
		}
	}
}
