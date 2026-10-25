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

#include <thread>
#include <chrono>

BaseDreamLink::BaseDreamLink(bool storageSupported) :
	storageSupported(storageSupported)
{
}

bool BaseDreamLink::storageEnabled()
{
	return storageSupported && config::UsePhysicalVmuMemory;
}

const char* BaseDreamLink::getIssueDescription() const
{
	return nullptr;
}

void BaseDreamLink::term()
{
	unregisterLink(true);

	// Invalidate internal data
    linkedBus = -1;
    linkedPortsMask = 0;
    connectedPortsMask = 0;
}

void BaseDreamLink::onGameStarted()
{
}

void BaseDreamLink::registerLink(int bus, u32 portsMask, LinkPriority priority)
{
	PrioritizedRegistry::Get().registerLink(shared_from_this(), bus, portsMask, priority);
}

void BaseDreamLink::unregisterLink(bool isTerminal)
{
	PrioritizedRegistry::Get().unregisterLink(this, isTerminal);
}

std::shared_ptr<maple_device> BaseDreamLink::createMapleDevice(int bus, int port)
{
	// Support basic controller with VMU and PuruPuru pack
	switch (port)
	{
		case 0: return std::make_shared<MapleLinkVmu>(MapleLink(shared_from_this(), bus, port));
		case 1: return std::make_shared<MapleLinkPuruPuru>(MapleLink(shared_from_this(), bus, port));
		case 5: return maple_Create(MapleDeviceType::MDT_SegaController);
		default: return std::make_shared<MapleLinkStub>();
	}
}

BaseDreamLink::PrioritizedRegistry::PrioritizedRegistry()
{
	EventManager::listen(Event::Start, EventHandler, this);
	EventManager::listen(Event::Terminate, EventHandler, this);
}

BaseDreamLink::PrioritizedRegistry::~PrioritizedRegistry()
{
	EventManager::unlisten(Event::Start, EventHandler, this);
	EventManager::unlisten(Event::Terminate, EventHandler, this);
}

BaseDreamLink::PrioritizedRegistry& BaseDreamLink::PrioritizedRegistry::Get()
{
	static PrioritizedRegistry inst;
	return inst;
}

void BaseDreamLink::PrioritizedRegistry::registerLink(
	const BaseDreamLink::Ptr& newDreamlink,
	int bus,
	u32 portsMask,
	LinkPriority priority
)
{
	if (newDreamlink->linkedBus == bus && newDreamlink->connectedPortsMask == portsMask)
		return;

	std::lock_guard<std::recursive_mutex> lock(mMutex);

	// Ensure this link is not currently established
	removeLinkFromRegistry(newDreamlink.get(), bus);

	if (isValidBus(bus))
	{
		newDreamlink->linkedBus = bus;
		newDreamlink->linkedPortsMask = portsMask;
		newDreamlink->connectedPortsMask = portsMask;

		std::list<BaseDreamLink::Ptr>& priorities = mRegistry[bus];

		// Establish local registry link
		// Since there is only high/low priorities, either put in front or back
		if (priority == LinkPriority::HIGH)
		{
			if (!priorities.empty())
			{
				// Remove connected port flags since this link now takes precedence
				for (BaseDreamLink::Ptr& existingLink : priorities)
				{
					const u32 prev = existingLink->connectedPortsMask;
					existingLink->connectedPortsMask = existingLink->connectedPortsMask & ~portsMask;
					if (existingLink->connectedPortsMask == 0)
					{
						if (prev != existingLink->connectedPortsMask)
						{
							// No longer connected to anything
							existingLink->disconnect();
						}
					}
				}
			}

			priorities.push_front(newDreamlink);
		}
		else
		{
			if (!priorities.empty())
			{
				// Remove connected port flags from this link since existing items take precedence
				for (BaseDreamLink::Ptr& existingLink : priorities)
				{
					newDreamlink->connectedPortsMask = newDreamlink->connectedPortsMask & ~existingLink->connectedPortsMask;
					if (newDreamlink->connectedPortsMask == 0)
					{
						// No longer connected to anything
						newDreamlink->disconnect();
						break;
					}
				}
			}

			priorities.push_back(newDreamlink);
		}

		establishInMapleLinkRegistry(newDreamlink, bus, newDreamlink->connectedPortsMask);
	}
	else
	{
		// Not a valid bus, so this should not be connected
		newDreamlink->linkedBus = -1;
		newDreamlink->disconnect();
	}

	MapleLinkRegistry::Get().commitChanges();
}

void BaseDreamLink::PrioritizedRegistry::unregisterLink(BaseDreamLink* dreamlink, bool isTerminal)
{
	if (!isValidBus(dreamlink->linkedBus))
		return;

	std::lock_guard<std::recursive_mutex> lock(mMutex);

	dreamlink->linkedBus = -1;

	if (!isTerminal)
	{
		dreamlink->disconnect();
	}

	// Remove everything previously established for this controller
	removeLinkFromRegistry(dreamlink);

	MapleLinkRegistry::Get().commitChanges();
}

void BaseDreamLink::PrioritizedRegistry::eventHandler(Event event)
{
	// Perform events for each link
	std::lock_guard<std::recursive_mutex> lock(mMutex);
	for (std::list<Ptr>& priorities : mRegistry)
	{
		for (const Ptr& ptr : priorities)
		{
			switch (event)
			{
			case Event::Start:
				ptr->onGameStarted();
				break;
			case Event::Terminate:
				ptr->onGameTermination();
				break;
			default:
				break;
			}
		}
	}
}

void BaseDreamLink::PrioritizedRegistry::EventHandler(Event event, void *p)
{
	PrioritizedRegistry* reg = reinterpret_cast<PrioritizedRegistry*>(p);
	reg->eventHandler(event);
}

void BaseDreamLink::PrioritizedRegistry::establishInMapleLinkRegistry(const BaseDreamLink::Ptr& dreamlink, int bus, u32 portsMask)
{
	if (MapleLinkRegistry::Get().registerLinks(dreamlink, bus, portsMask) > 0)
	{
		dreamlink->connect();
	}
}

void BaseDreamLink::PrioritizedRegistry::removeLinkFromRegistry(const BaseDreamLink* dreamlink, int newBus)
{
	// Remove from MapleLinkRegistry
	MapleLinkRegistry::Get().unregisterLinks(dreamlink);

	for (int bus = 0; bus < mRegistry.size(); ++bus)
	{
		std::list<Ptr>& priorities = mRegistry[bus];
		std::list<Ptr>::iterator iter = priorities.begin();
		while (iter != priorities.end())
		{
			if (iter->get() != dreamlink)
			{
				++iter;
				continue;
			}

			iter = priorities.erase(iter);

			if (bus != newBus && iter != priorities.end())
			{
				// Establish "new" priorities into MapleLinkRegistry
				u32 availablePorts = ALL_PORTS_MASK;
				for (std::list<Ptr>::iterator innerIter = iter; innerIter != priorities.end(); ++innerIter)
				{
					const Ptr& link = *innerIter;
					link->connectedPortsMask = availablePorts & link->linkedPortsMask;
					if (link->connectedPortsMask != 0)
					{
						establishInMapleLinkRegistry(link, bus, link->connectedPortsMask);
						availablePorts = availablePorts & ~link->connectedPortsMask;
					}
				}
			}
		}
	}
}
