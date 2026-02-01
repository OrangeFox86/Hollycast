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

MapleLinkRegistry& MapleLinkRegistry::Get()
{
    static MapleLinkRegistry singleton;
    return singleton;
}

MapleLink::Ptr MapleLinkRegistry::getMapleLink(int bus, int port)
{
    if (!DreamLink::isValidBus(bus) || !DreamLink::isValidPort(port))
        return nullptr;

    std::lock_guard<std::mutex> lock(mutex);

    return links[bus][port];
}

bool MapleLinkRegistry::storageEnabled()
{
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& perPort : links)
    {
        for (MapleLink::Ptr& mapleLink : perPort)
        {
            if (mapleLink && mapleLink->dreamlink->storageEnabled())
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

    links[bus][port] = std::make_shared<MapleLink>(dreamlink, bus, port);
}

void MapleLinkRegistry::unregisterLink(const DreamLink* dreamlink)
{
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& perPort : links)
    {
        for (MapleLink::Ptr& mapleLink : perPort)
        {
            if (mapleLink && mapleLink->dreamlink.get() == dreamlink)
                mapleLink.reset();
        }
    }
}
