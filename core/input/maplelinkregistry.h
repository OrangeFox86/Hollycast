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
#include <optional>

class MapleLinkRegistry
{
    // This class is tightly coupled with BaseDreamLink as this serves as the "global" registration point for it
    friend class BaseDreamLink;

protected:
    //! Default constructor (default)
    MapleLinkRegistry() = default;
	//! Copy constructor (deleted)
    MapleLinkRegistry(const MapleLinkRegistry&) = delete;

public:
	//! Destructor (default)
    ~MapleLinkRegistry() = default;

    //! @return the singleton object
    static MapleLinkRegistry& Get();

	//! Static retrieval function for the designated MapleLink at the given bus/port
	//! @param[in] bus Dreamcast bus index [0,3]
	//! @param[in] port Peripheral port index [0,5]
	//! @return the MapleLink at the given bus/port if one is installed
	//! @return nullptr otherwise
	static inline std::optional<MapleLink> GetMapleLink(int bus, int port) {
		return Get().getMapleLink(bus, port);
	}

	//! Static retrieval function for storage enabled flag
	//! @return true if storage is enabled on any active maple link
	static inline bool StorageEnabled() {
		return Get().storageEnabled();
	}

    //! Non-static retrieval function for the designated MapleLink at the given bus/port
	//! @see GetMapleLink
    std::optional<MapleLink> getMapleLink(int bus, int port);

	//! Non-static retrieval function for storage enabled flag
	//! @see  StorageEnabled
	bool storageEnabled();

private:
    //! Called by a BaseDreamLink in order to register it in this registry
	//! @post call commitChanges() once all registration/unregistration is complete
    //! @param[in] dreamlink Smart pointer to the dreamlink
	//! @param[in] bus Dreamcast bus index [0,3]
	//! @param[in] portsMask Peripheral port mask (up to DreamLink::ALL_PORTS_MASK)
    std::size_t registerLinks(const DreamLink::Ptr& dreamlink, u32 bus, u32 portsMask);

    //! Called by a BaseDreamLink in order to unregister all entries from this registry
	//! @post call commitChanges() once all registration/unregistration is complete
    //! @param[in] dreamlink Raw pointer to a dreamlink
    std::size_t unregisterLinks(const DreamLink* dreamlink);

	//! Commit all registration/unregistration changes
	void commitChanges();

private:
    //! Mutex serializing access to links and portReconnectMasks
    std::mutex mutex;
    //! Registry of links by [bus][port]
    std::array<std::array<DreamLink::WPtr, MAPLE_DEVS_PER_PORT>, MAPLE_PORTS> links;
	//! For each bus, the port mask for ports that require maple reconnection
	std::array<u32, MAPLE_PORTS> portReconnectMasks;
};
