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

BaseDreamLink::~BaseDreamLink()
{
	stopConnectionWorkerThread();
}

bool BaseDreamLink::storageEnabled()
{
	return storageSupported && config::UsePhysicalVmuMemory;
}

bool BaseDreamLink::isGameRunning() const
{
	return PrioritizedRegistry::Get().isGameRunning();
}

const char* BaseDreamLink::getIssueDescription() const
{
	return nullptr;
}

void BaseDreamLink::term()
{
	unregisterLink(true);
	stopConnectionWorkerThread();

	// Invalidate internal data
    linkedBus = -1;
    linkedPortsMask = 0;
    connectedPortsMask = 0;
}

void BaseDreamLink::onGameStarted()
{
}

void BaseDreamLink::registerLink(int bus, u32 portsMask)
{
	PrioritizedRegistry::Get().registerLink(shared_from_this(), bus, portsMask);
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

void BaseDreamLink::asyncRetryConnect()
{
	std::lock_guard<std::mutex> lock(connectionMutex);

	// Only allow the request if Connect was last executed internally
	if (lastConnectRequest == ConnectionWorkType::Connect)
	{
		asyncConnection(ConnectionWorkType::Connect, false);
	}
}

void BaseDreamLink::asyncConnection(const ConnectionWorkType& type, bool getLock)
{
	std::unique_lock<std::mutex> lock(connectionMutex, std::defer_lock);

	if (getLock)
	{
		lock.lock();
	}

	if (connectionWorkerShutdown)
	{
		return;
	}

	// Lazily initialize the worker thread on first use
	if (!connectionWorker)
	{
		connectionWorker = std::make_unique<std::thread>([this]() { connectionWorkerThread(); });
	}

	lastConnectRequest = type;
	connectionWorkQueue.push_back(type);
	connectionCondVar.notify_one();
}

void BaseDreamLink::connectionWorkerThread()
{
	// The predicate which returns true if item is available to pop or thread shutdown is requested
	auto pred = [this](){ return !connectionWorkQueue.empty() || connectionWorkerShutdown; };

	while (true)
	{
		ConnectionWorkType work;

		{
			std::unique_lock<std::mutex> lock(connectionMutex);

			// Wait for work or shutdown signal
			connectionCondVar.wait(lock, pred);

			if (connectionWorkerShutdown)
			{
				return;  // Shutdown, exit thread
			}

			// Only the back item will ever take precedence
			work = connectionWorkQueue.back();
			connectionWorkQueue.clear();
		}

		// Execute the connection work without holding the lock
		switch (work)
		{
		case ConnectionWorkType::Connect:
			while (true)
			{
				if (connectionWorkerShutdown)
				{
					return;  // Shutdown, exit thread
				}

				connect();
				if (isConnected())
				{
					break;
				}
				else
				{
					const char* issueDesc = getIssueDescription();
					if (issueDesc != nullptr)
					{
						NOTICE_LOG(INPUT, "DreamLink connection failed: %s", issueDesc);
						break;
					}
				}

				NOTICE_LOG(INPUT, "DreamLink connection failed; retrying connection in 1 second");

				// Wait for 1 second while checking for new events or shutdown
				{
					std::unique_lock<std::mutex> lock(connectionMutex);
					if (connectionCondVar.wait_for(lock, std::chrono::seconds(1), pred))
					{
						break; // New event
					}
				}
			}
			break;
		case ConnectionWorkType::Disconnect:
			disconnect();
			break;
		}
	}
}

void BaseDreamLink::stopConnectionWorkerThread()
{
	std::unique_ptr<std::thread> workerThread;

	{
		std::lock_guard<std::mutex> lock(connectionMutex);
		connectionWorkerShutdown = true;
		connectionCondVar.notify_all();
		workerThread = std::move(connectionWorker);
	}

	if (workerThread)
	{
		workerThread->join();
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
	const BaseDreamLink::Ptr& dreamlink,
	int bus,
	u32 portsMask
)
{
	if (dreamlink->linkedBus == bus && dreamlink->connectedPortsMask == portsMask)
		return;

	std::lock_guard<std::recursive_mutex> lock(mMutex);

	// Ensure this link is not currently established
	removeLinkFromRegistry(dreamlink.get(), bus);

	if (isValidBus(bus))
	{
		dreamlink->linkedBus = bus;
		dreamlink->linkedPortsMask = portsMask;
		dreamlink->connectedPortsMask = portsMask;

		std::list<BaseDreamLink::Ptr>& priorities = mRegistry[bus];

		if (!priorities.empty())
		{
			// Remove connected port flags since this link now takes precedence
			for (BaseDreamLink::Ptr& link : priorities)
			{
				const u32 prev = link->connectedPortsMask;
				link->connectedPortsMask = link->connectedPortsMask & ~portsMask;
				if (link->connectedPortsMask == 0)
				{
					if (prev != link->connectedPortsMask)
					{
						// No longer connected to anything
						link->asyncConnection(ConnectionWorkType::Disconnect);
					}
				}
			}
		}

		// Establish local registry link
		priorities.push_front(dreamlink);

		establishInMapleLinkRegistry(dreamlink, bus, portsMask);
	}
	else
	{
		// Not a valid bus, so this should not be connected
		dreamlink->linkedBus = -1;
		dreamlink->asyncConnection(ConnectionWorkType::Disconnect);
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
		dreamlink->asyncConnection(ConnectionWorkType::Disconnect);
	}

	// Remove everything previously established for this controller
	removeLinkFromRegistry(dreamlink);

	MapleLinkRegistry::Get().commitChanges();
}

bool BaseDreamLink::PrioritizedRegistry::isGameRunning() const
{
	return mIsGameRunning;
}

void BaseDreamLink::PrioritizedRegistry::eventHandler(Event event)
{
	// Set GameStarted flag
	switch (event)
	{
	case Event::Start:
		mIsGameRunning.store(true);
		break;
	case Event::Terminate:
		mIsGameRunning.store(false);
		break;
	default:
		break;
	}

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
		dreamlink->asyncConnection(ConnectionWorkType::Connect);
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
					const u32 ports = availablePorts & link->linkedPortsMask;
					if (ports != 0)
					{
						establishInMapleLinkRegistry(link, bus, ports);
						availablePorts = availablePorts & ~ports;
					}
				}
			}
		}
	}
}
