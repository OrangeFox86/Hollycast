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
#include "maplelink.h"
#include "maplelinkregistry.h"
#include "cfg/option.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_devs.h"
#include "oslib/oslib.h"

#include <memory>

MapleLink::MapleLink(const DreamLink::Ptr& dreamlink, u32 bus, u32 port) :
	dreamlink(dreamlink),
	bus(bus),
	port(port)
{}

MapleLink::MapleLink(DreamLink::Ptr&& dreamlink, u32 bus, u32 port) :
	dreamlink(std::move(dreamlink)),
	bus(bus),
	port(port)
{}

bool MapleLink::send(const MapleMsg& msg)
{
	return dreamlink->send(msg);
}

bool MapleLink::sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg)
{
	return dreamlink->sendReceive(txMsg, rxMsg);
}

bool MapleLink::storageEnabled()
{
	return dreamlink->storageEnabled();
}

bool MapleLink::isConnected()
{
	return dreamlink->isConnected();
}

std::shared_ptr<maple_device> MapleLink::createMapleDevice()
{
	return dreamlink->createMapleDevice(bus, port);
}

static bool relayMapleLink(u8 bus, u8 port, const MapleMsg& msg)
{
	auto link = MapleLinkRegistry::GetMapleLink(bus, port);
	if (!link)
		return true;
	else
		return link->send(msg);
}

u32 MapleLinkVmu::dma(u32 cmd)
{
	auto link = MapleLinkRegistry::GetMapleLink(bus_id, bus_port);
	return dma(link, cmd);
}

u32 MapleLinkVmu::dma(std::optional<MapleLink>& link, u32 cmd)
{
	u32 rv = maple_sega_vmu::dma(cmd);
	if (link && inMsg && inMsg->size > 0)
	{
		u32 function = inMsg->readData<u32>(0);
		if ((cmd == MDCF_BlockWrite && function == MFID_2_LCD) || cmd == MDCF_SetCondition)
		{
			link->send(*inMsg);
		}
	}
	return rv;
}

u32 MapleLinkPuruPuru::dma(u32 cmd)
{
	auto link = MapleLinkRegistry::GetMapleLink(bus_id, bus_port);
	return dma(link, cmd);
}

u32 MapleLinkPuruPuru::dma(std::optional<MapleLink>& link, u32 cmd)
{
	u32 rv = maple_sega_purupuru::dma(cmd);
	if (link && inMsg)
	{
		if (cmd == MDCF_BlockWrite || cmd == MDCF_SetCondition)
		{
			link->send(*inMsg);
		}
	}
	return rv;
}
