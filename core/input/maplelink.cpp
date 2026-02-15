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
#include "oslib/i18n.h"
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

bool MapleLink::storageEnabled() const
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

void MapleLinkDevice::deserializeVmu(
	Deserializer &deser,
	MapleLink& link,
	maple_base& dev,
	maple_sega_vmu& vmu,
	bool usingExternalMemory
)
{
	bool dataMatches = true;

	u8 currentKnownFlash[sizeof(vmu.flash_data)];
	memcpy(&currentKnownFlash[0], &vmu.flash_data[0], sizeof(currentKnownFlash));
	u8 currentAccessedBlocks[sizeof(vmu.accessed_blocks)];
	memcpy(&currentAccessedBlocks[0], &vmu.accessed_blocks[0], sizeof(currentAccessedBlocks));
	vmu.maple_sega_vmu::deserialize(deser);

	// Determine if the device needs to be reconnected
	if (usingExternalMemory) {
		if (!vmu.accessed_blocks_valid) {
			// Legacy save file - cause reconnect
			dataMatches = false;
			os_notify(
				i18n::T("ATTENTION: External storage differs from VMU memory in loaded state"),
				6000,
				i18n::T("VMU will appear to reconnect in-game")
			);
		} else if (vmu.save_tick > 0 && vmu.save_tick < (vmu.last_write_tick + (SH4_MAIN_CLOCK / 10))) {
			// Write was in progress during save state - cause reconnect
			dataMatches = false;
			os_notify(
				i18n::T("ATTENTION: VMU write was in progress during save"),
				6000,
				i18n::T("VMU will appear to reconnect in-game")
			);
		} else {
			for (std::size_t i = sizeof(currentAccessedBlocks); i > 0; --i) {
				const u8 block = (i - 1);
				const bool isCached = (vmu.accessed_blocks[block] != 0);
				if (isCached) {
					if (!currentAccessedBlocks[block]) {
						// Try up to 4 times to read this block
						for (u32 j = 0; j < 4; ++j) {
							MapleMsg msg{};
							msg.command = MDCF_BlockRead;
							// Hardware bus number will be overridden by link->sendReceive()
							msg.destAP = (dev.bus_id << 6) | (1 << dev.bus_port);
							msg.originAP = dev.bus_id << 6;
							msg.pushData(MFID_1_Storage);
							const u32 locationWord = (block & 0xFF) << 24;
							msg.pushData(locationWord);

							MapleMsg rxMsg;
							if (link.sendReceive(msg, rxMsg) && rxMsg.size >= 130) {
								// Something read!
								memcpy(&currentKnownFlash[block * 512], &rxMsg.data[8], 512);
								currentAccessedBlocks[block] = true;
								break;
							}
						}
					}

					if (
						!currentAccessedBlocks[block] ||
						(memcmp(&currentKnownFlash[block * 512], &vmu.flash_data[block * 512], 512) != 0)
					) {
						os_notify(
							i18n::T("ATTENTION: External storage differs from VMU memory in loaded state"),
							6000,
							i18n::T("VMU will appear to reconnect in-game")
						);
						dataMatches = false;
						break;
					}
				}
			}
		}
	}

	if (dataMatches) {
		mirrorLcd(link, dev, vmu);
	} else {
		dev.requestReconnect();
		// Reset the virtual VMU data
		vmu.accessed_blocks_valid = true;
		memset(&vmu.flash_data[0], 0, sizeof(vmu.flash_data));
		memset(&vmu.accessed_blocks[0], 0, sizeof(vmu.accessed_blocks));
	}
}

void MapleLinkDevice::mirrorLcd(MapleLink& link, maple_base& dev, maple_sega_vmu& vmu)
{
	// Set the screen to what is in state
	MapleMsg msg;
	msg.command = MDCF_BlockWrite;
	msg.destAP = (dev.bus_id << 6) | (1 << dev.bus_port);
	msg.originAP = dev.bus_id << 6;
	msg.pushData(MFID_2_LCD);
	msg.pushData(0);    // PT, phase, block#
	msg.pushData(vmu.lcd_data);
	link.send(msg);
}

MapleLinkVmu::MapleLinkVmu(const MapleLink& link) : MapleLinkDeviceBase<maple_sega_vmu>(link)
{}

void MapleLinkVmu::OnSetup()
{
	if (link.storageEnabled())
	{
		maple_sega_vmu::OnSetup();
		return;
	}

	// Ensure file is not being used
	if (file != nullptr) {
		std::fclose(file);
		file = nullptr;
	}

	initializeVmu(); // Start with a valid VMU memory state
	memset(lcd_data, 0, sizeof(lcd_data));
	accessed_blocks_valid = true;
	memset(accessed_blocks, 0, sizeof(accessed_blocks));
	last_write_tick = 0;
	save_tick = 0;
	fullSaveNeeded = false;
}

u32 MapleLinkVmu::dma(u32 cmd)
{
	u32 rv = maple_sega_vmu::dma(cmd);
	if (inMsg && inMsg->size > 0)
	{
		u32 function = inMsg->readData<u32>(0);
		if ((cmd == MDCF_BlockWrite && function == MFID_2_LCD) || cmd == MDCF_SetCondition)
		{
			link.send(*inMsg);
		}
	}
	return rv;
}

void MapleLinkVmu::deserialize(Deserializer& deser)
{
	maple_sega_vmu::deserialize(deser);
	mirrorLcd();
}

MapleLinkPuruPuru::MapleLinkPuruPuru(const MapleLink& link) : MapleLinkDeviceBase<maple_sega_purupuru>(link)
{}

u32 MapleLinkPuruPuru::dma(u32 cmd)
{
	u32 rv = maple_sega_purupuru::dma(cmd);
	if (inMsg)
	{
		if (cmd == MDCF_BlockWrite || cmd == MDCF_SetCondition)
		{
			link.send(*inMsg);
		}
	}
	return rv;
}
