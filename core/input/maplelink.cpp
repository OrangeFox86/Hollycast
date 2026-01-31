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
#include "cfg/option.h"
#include "hw/maple/maple_if.h"
#include "oslib/oslib.h"

std::array<std::array<std::list<MapleLink::Ptr>, 2>, 4> MapleLink::Links;
std::mutex MapleLink::Mutex;

namespace
{
struct GameState
{
	GameState()
	{
		EventManager::listen(Event::Start, [](Event, void*) {
			started = true;
		});
		EventManager::listen(Event::Terminate, [](Event, void*) {
			started = false;
		});
	}
	static bool started;
	static GameState instance;
};
bool GameState::started;
GameState GameState::instance;
}

//! A specialized VMU which interfaces with a MapleLink's VMU, including read/write operations
struct MapleLinkVmu : public maple_sega_vmu
{
	bool cachedBlocks[256]; //!< Set to true for block that has been loaded/written
	bool userNotified = false;

	void OnSetup() override
	{
		// All data must be re-read
		memset(cachedBlocks, 0, sizeof(cachedBlocks));

		// Ensure file is not being used
		if (file != nullptr) {
			std::fclose(file);
			file = nullptr;
		}

		memset(flash_data, 0, sizeof(flash_data));
		memset(lcd_data, 0, sizeof(lcd_data));
	}

	bool fullSave() override
	{
		// Skip virtual save when using MapleLink VMU
		DEBUG_LOG(MAPLE, "Full save ignored for MapleLink VMU");
		return true;
	}

	void serialize(Serializer& ser) const override {
		throw Serializer::Exception("Can't save linked VMU data");
	}

	void deserialize(Deserializer& deser) override
	{
		// Ignore the VMU data from the loaded state
		u8 savedData[sizeof(flash_data)];
		memcpy(savedData, flash_data, sizeof(savedData));
		maple_sega_vmu::deserialize(deser);
		memcpy(flash_data, savedData, sizeof(savedData));
	}

	MapleLink::Ptr getMapleLink()
	{
		MapleLink::Ptr link = MapleLink::GetMapleLink(bus_id, bus_port);
		if (link == nullptr)
			ERROR_LOG(MAPLE, "MapleLinkVmu[%s]: MapleLink is null", logical_port);
		return link;
	}

	MapleDeviceRV readBlock(unsigned block)
	{
		if (cachedBlocks[block])
			return MDRS_JVSNone;

		MapleLink::Ptr link = getMapleLink();
		if (link == nullptr)
			return MDRS_JVSNone;

		MapleMsg txMsg;
		txMsg.command = MDCF_BlockRead;
		txMsg.originAP = inMsg->originAP;
		txMsg.destAP = inMsg->destAP;
		txMsg.pushData(MFID_1_Storage);
		txMsg.pushData<u32>(block << 24); // (BE) partition #, phase, block #
		MapleMsg rxMsg;
		if (link->sendReceive(txMsg, rxMsg) && rxMsg.size == 130)
		{
			DEBUG_LOG(MAPLE, "MapleLinkVmu[%s]: read block %d", logical_port, block);
			memcpy(&flash_data[block * 4 * 128], &rxMsg.data[8], 4 * 128);
			cachedBlocks[block] = true;
		}
		else {
			ERROR_LOG(MAPLE, "Failed to read VMU %s: I/O error", logical_port);
			return MDRE_FileError; // I/O error
		}
		return MDRS_JVSNone;
	}

	u32 dma(u32 cmd) override
	{
		// Physical VMU logic
		if (dma_count_in >= 4)
		{
			const u32 functionId = inMsg->readData<u32>(0);

			if (functionId == MFID_1_Storage)
			{
				switch (cmd)
				{
				case MDCF_BlockWrite:
				{
					if (!userNotified)
					{
						os_notify("ATTENTION: You are saving to a physical VMU", 6000,
								"Do not disconnect the VMU or close the game");
						userNotified = true;
					}
					MapleLink::Ptr link = getMapleLink();
					if (link == nullptr)
						return MDRE_FileError;
					MapleMsg rxMsg;
					if (!link->sendReceive(*inMsg, rxMsg)) {
						ERROR_LOG(MAPLE, "Failed to write VMU %s: I/O error", logical_port);
						return MDRE_FileError;
					}
					if (rxMsg.command != MDRS_DeviceReply)
						return rxMsg.command;
					cachedBlocks[inMsg->data[7]] = true;
					DEBUG_LOG(MAPLE, "MapleLinkVmu[%s]: write block %d", logical_port, inMsg->data[7]);
					break;
				}

				case MDCF_BlockRead:
				{
					u8 block = inMsg->data[7];
					MapleDeviceRV rc = readBlock(block);
					if (rc != MDRS_JVSNone)
						return rc;
					break;
				}

				case MDCF_GetMediaInfo:
					// block 255 contains the media info
					readBlock(255);
					break;

				case MDCF_GetLastError:
				{
					MapleLink::Ptr link = getMapleLink();
					if (link == nullptr)
						return MDRE_FileError;
					if (!link->handleGetLastError(*inMsg)) {
						ERROR_LOG(MAPLE, "MapleLinkVmu[%s]::GetLastError: I/O error", logical_port);
						return MDRE_FileError;
					}
					break;
				}

				default:
					// do nothing
					break;
				}
			}
		}
		return maple_sega_vmu::dma(cmd);
	}

	bool linkStatus() override
	{
		auto link = MapleLink::GetMapleLink(bus_id, bus_port);
		if (link == nullptr)
			return false;
		return link->isConnected();
	}
};

std::size_t MapleLink::activeLinkCount(int bus) const
{
	std::size_t count = 0;
	if (bus >= 0 && bus < (int)Links.size())
	{
		std::lock_guard<std::mutex> _(Mutex);
		for (const std::list<MapleLink::Ptr>& list : Links[bus]) {
			if (!list.empty() && list.front().get() == this)
				++count;
		}
	}
	return count;
}

bool MapleLink::isGameStarted() const {
	return GameState::started;
}

void MapleLink::registerLink(int bus, int port)
{
	if (bus >= 0 && bus < (int)Links.size()
			&& port >= 0 && port < (int)Links[0].size())
	{
		if (this->bus != bus) {
			this->bus = bus;
			this->ports = 0;
		}
		this->ports |= 1 << port;
		std::lock_guard<std::mutex> _(Mutex);
		Links[bus][port].push_front(shared_from_this());
	}
}
void MapleLink::unregisterLink(int bus, int port)
{
	if (bus >= 0 && bus < (int)Links.size()
			&& port >= 0 && port < (int)Links[0].size())
	{
		if (this->bus != bus) {
			this->bus = -1;
			this->ports = 0;
		}
		else {
			this->ports &= ~(1 << port);
		}
		std::lock_guard<std::mutex> _(Mutex);
		Links[bus][port].remove_if([this](const Ptr& item) { return item.get() == this; });
	}
}

bool MapleLink::StorageEnabled()
{
	std::lock_guard<std::mutex> _(Mutex);
	for (const auto& ports : Links)
	{
		for (const std::list<MapleLink::Ptr>& list : ports)
			if (!list.empty() && list.front()->storageEnabled())
				return true;
	}
	return false;
}

BaseMapleLink::BaseMapleLink(bool storageSupported)
	: storageSupported(storageSupported)
{
	EventManager::listen(Event::LoadState, eventHandler, this);
	EventManager::listen(Event::Start, eventHandler, this);
	EventManager::listen(Event::Terminate, eventHandler, this);
	if (isGameStarted())
		vmuStorage = storageSupported && config::UsePhysicalVmuMemory;
}

BaseMapleLink::~BaseMapleLink()
{
	EventManager::unlisten(Event::LoadState, eventHandler, this);
	EventManager::unlisten(Event::Start, eventHandler, this);
	EventManager::unlisten(Event::Terminate, eventHandler, this);
}

void BaseMapleLink::gameStarted() {
	vmuStorage = storageSupported && config::UsePhysicalVmuMemory && isConnected();
}

std::shared_ptr<maple_device> BaseMapleLink::createMapleDevice(MapleDeviceType type) {
	if (type == MapleDeviceType::MDT_SegaVMU && storageEnabled())
		return std::make_shared<MapleLinkVmu>();
	return maple_Create(type);
}

void BaseMapleLink::eventHandler(Event event, void *p)
{
	BaseMapleLink *self = (BaseMapleLink *)p;
	switch (event)
	{
	case Event::Start:
		self->gameStarted();
		break;
	case Event::Terminate:
		self->gameTermination();
		break;
	case Event::LoadState:
		if (self->vmuStorage) {
			WARN_LOG(INPUT, "State loaded but VMU has storage enabled");
			self->disableStorage();
		}
		break;
	default:
		break;
	}
}

void BaseMapleLink::disableStorage()
{
	if (!vmuStorage)
		return;
	vmuStorage = false;
	if (isGameStarted())
	{
		emu.run([bus=this->bus, ports=this->ports]() {
			if (bus != -1 && (ports & 1))
				maple_ReconnectDevice(bus, 0);
			if (bus != -1 && (ports & 2))
				maple_ReconnectDevice(bus, 1);
		});
	}
}

bool BaseMapleLink::storageEnabled()
{
	if (!isConnected())
		return false;
	if (!isGameStarted())
		return storageSupported && config::UsePhysicalVmuMemory;
	else
		return vmuStorage;
}

bool BaseMapleLink::handleGetLastError(const MapleMsg&)
{
	// Just acknowledge by default
	return true;
}
