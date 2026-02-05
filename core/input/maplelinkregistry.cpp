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

std::optional<MapleLink> MapleLinkRegistry::getMapleLink(int bus, int port)
{
    if (!DreamLink::isValidBus(bus) || !DreamLink::isValidPort(port))
        return std::nullopt;

    DreamLink::Ptr link;

    {
        std::lock_guard<std::mutex> lock(mutex);
        link = links[bus][port].lock();
    }

    if (!link)
        return std::nullopt;

    return MapleLink(std::move(link), bus, port);
}

bool MapleLinkRegistry::storageEnabled()
{
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& perPort : links)
    {
        for (DreamLink::WPtr& wptr : perPort)
        {
            DreamLink::Ptr link = wptr.lock();
            if (link && link->storageEnabled())
                return true;
        }
    }

    return false;
}

void MapleLinkRegistry::registerLink(const DreamLink::Ptr& dreamlink, u32 bus, u32 port)
{
    if (!DreamLink::isValidBus(bus) || !DreamLink::isValidPort(port))
        return;

    std::lock_guard<std::mutex> lock(mutex);

    DreamLink::Ptr link = links[bus][port].lock();

    const bool reconnect = (link && link.get() != dreamlink.get() && dreamlink->isGameStarted());

    links[bus][port] = dreamlink;

    if (reconnect)
    {
		emu.run([bus, port]() {
			maple_ReconnectDevice(bus, port);
		});
    }
}

void MapleLinkRegistry::unregisterLink(const DreamLink* dreamlink)
{
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
                const bool reconnect = link->isGameStarted();
                wptr.reset();
                link.reset();

                if (reconnect)
                {
                    emu.run([bus, port]() {
                        maple_ReconnectDevice(bus, port);
                    });
                }
            }
        }
    }
}
