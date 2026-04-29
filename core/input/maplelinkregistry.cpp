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

#include "maplelinkregistry.h"

#include "hw/maple/maple_if.h"
#include "emulator.h"

MapleLinkRegistry& MapleLinkRegistry::Get()
{
    static MapleLinkRegistry singleton;
    return singleton;
}

std::optional<MapleLink> MapleLinkRegistry::getMapleLink(int bus, int port, int linkBusOffset)
{
    if (!DreamLink::isValidBus(bus) || !DreamLink::isValidPort(port))
        return std::nullopt;

    DreamLink::Ptr link;

    {
        std::lock_guard<std::mutex> lock(mutex);
        link = links[bus + linkBusOffset][port].lock();
    }

    if (!link)
        return std::nullopt;

    return MapleLink(std::move(link), bus, port);
}

std::size_t MapleLinkRegistry::registerLinks(const DreamLink::Ptr& dreamlink, u32 bus, u32 portsMask)
{
    portsMask &= DreamLink::ALL_PORTS_MASK;
    std::size_t numRegistered = 0;

    if (!DreamLink::isValidBus(bus) || (portsMask == 0))
    {
        return numRegistered;
    }

    std::lock_guard<std::mutex> lock(mutex);

    numRegistered = DreamLink::forEachPort(
        portsMask,
        [this, &dreamlink, bus](int port)
        {
            DreamLink::Ptr link = links[bus][port].lock();

            if (link.get() != dreamlink.get() && dreamlink->isGameRunning())
            {
                portReconnectMasks[bus] |= (1 << port);
            }

            links[bus][port] = dreamlink;
        }
    );

    return numRegistered;
}

std::size_t MapleLinkRegistry::unregisterLinks(const DreamLink* dreamlink)
{
    std::size_t numUnregistered = 0;

    std::lock_guard<std::mutex> lock(mutex);

    for (std::size_t bus = 0; bus < links.size(); ++bus)
    {
        auto& perPort = links[bus];
        for (std::size_t port = 0; port < perPort.size(); ++port)
        {
            DreamLink::WPtr& wptr = perPort[port];
            DreamLink::Ptr link = wptr.lock();
            if (link && link.get() == dreamlink)
            {
                const bool reconnect = link->isGameRunning();
                wptr.reset();
                link.reset();

                ++numUnregistered;

                if (reconnect)
                {
                    portReconnectMasks[bus] |= (1 << port);
                }
            }
        }
    }

    return numUnregistered;
}

void MapleLinkRegistry::commitChanges()
{
    std::lock_guard<std::mutex> lock(mutex);

    bool changeDetected = false;

    for (const u32& portsMask : portReconnectMasks)
    {
        if (portsMask != 0)
        {
            changeDetected = true;
            break;
        }
    }

    if (!changeDetected)
        return;

    std::array<u32, MAPLE_PORTS> changes = portReconnectMasks;
    portReconnectMasks.fill(0);

    emu.run([changes]() {
        for (int bus = 0; bus < changes.size(); ++bus)
        {
            u32 portsMask = changes[bus];
            DreamLink::forEachPort(
                portsMask,
                [bus, portsMask](int port)
                {
                    maple_ReconnectDevice(bus, port);
                }
            );
        }
    });
}
