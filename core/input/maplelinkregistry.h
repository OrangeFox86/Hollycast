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
#include "maplelink.h"

#include <array>
#include <mutex>

class MapleLinkRegistry
{
    // This class is tightly coupled with BaseDreamLink as this serves as the "global" registration point for it
    friend class BaseDreamLink;

protected:
    MapleLinkRegistry() = default;
    MapleLinkRegistry(const MapleLinkRegistry&) = delete;

public:
    ~MapleLinkRegistry() = default;

    static MapleLinkRegistry& Get();
	static inline MapleLink::Ptr GetMapleLink(int bus, int port) {
		return Get().getMapleLink(bus, port);
	}
	static inline bool StorageEnabled() {
		return Get().storageEnabled();
	}

    MapleLink::Ptr getMapleLink(int bus, int port);

	bool storageEnabled();

private:
    void registerLink(const DreamLink::Ptr& dreamlink, u32 bus, u32 port);
    void unregisterLink(const DreamLink* dreamlink);

private:
    std::mutex mutex;
    std::array<std::array<MapleLink::Ptr, MAPLE_DEVS_PER_PORT>, MAPLE_PORTS> links;
};
