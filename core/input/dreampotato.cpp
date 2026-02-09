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
#include "types.h"
#include <asio.hpp> // Must be included first to avoid winsock issues on Windows
#include "dreampotato.h"
#include "dreamlink.h"
#include "maplelink.h"
#include "maplelinkregistry.h"
#include "emulator.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include <memory>
#include <array>
#include <iomanip>

asio::error_code sendMsg(const MapleMsg& msg, asio::ip::tcp::iostream& stream)
{
	static char buffer[1024 * 3 + 2];
	char *p = buffer;
	p += sprintf(p, "%02X %02X %02X %02X", msg.command, msg.destAP, msg.originAP, msg.size);
	const u32 sz = msg.getDataSize();
	for (u32 i = 0; i < sz; i++)
		p += sprintf(p, " %02X", msg.data[i]);
	strcpy(p, "\r\n");
	p += 2;
	asio::ip::tcp::socket& sock = static_cast<asio::ip::tcp::socket&>(stream.socket());
	asio::error_code ec;
	asio::write(sock, asio::buffer(buffer, p - buffer), ec);
	return ec;
}

bool receiveMsg(MapleMsg& msg, std::istream& stream)
{
	std::string response;
	if (!std::getline(stream, response))
		return false;
	sscanf(response.c_str(), "%hhx %hhx %hhx %hhx", &msg.command, &msg.destAP, &msg.originAP, &msg.size);
	if ((msg.getDataSize() - 1) * 3 + 13 >= response.length())
		return false;
	for (unsigned i = 0; i < msg.getDataSize(); i++)
		sscanf(&response[i * 3 + 12], "%hhx", &msg.data[i]);
	return !stream.fail();
}

namespace dreampotato
{

//! A specialized VMU which interfaces with a MapleLink's VMU, including read/write operations
struct DreamPotatoVmu : public MapleLinkVmu
{
	bool cachedBlocks[256]; //!< Set to true for block that has been loaded/written
	bool userNotified = false;
	//! Switched to true on first memory read/write command
	bool storageLinked = false;

	bool usingExternalStorage() const override
	{
		return storageLinked;
	}

	void OnSetup() override
	{
		// All data must be re-read
		memset(cachedBlocks, 0, sizeof(cachedBlocks));

		auto link = getMapleLink();
		if (!link || !link->storageEnabled())
		{
			MapleLinkVmu::OnSetup();
			return;
		}

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
		auto link = getMapleLink();
		if (!link || !link->storageEnabled())
		{
			return MapleLinkVmu::fullSave();
		}

		// Skip virtual save when using MapleLink VMU
		DEBUG_LOG(MAPLE, "Full save ignored for MapleLink VMU");
		return true;
	}

	void serialize(Serializer& ser) const override {
		auto link = getMapleLink();
		if (!link || !link->storageEnabled())
		{
			MapleLinkVmu::serialize(ser);
			return;
		}

		throw Serializer::Exception("Can't save linked VMU data");
	}

	void deserialize(Deserializer& deser) override
	{
		auto link = getMapleLink();
		if (!link || !link->storageEnabled())
		{
			MapleLinkVmu::deserialize(deser);
			return;
		}

		// Ignore the VMU data from the loaded state
		u8 savedData[sizeof(flash_data)];
		memcpy(savedData, flash_data, sizeof(savedData));
		MapleLinkVmu::deserialize(deser);
		memcpy(flash_data, savedData, sizeof(savedData));
	}

	std::optional<MapleLink> getMapleLink() const
	{
		std::optional<MapleLink> link = MapleLinkRegistry::GetMapleLink(bus_id, bus_port);
		if (!link)
			ERROR_LOG(MAPLE, "DreamPotatoVmu[%s]: MapleLink is null", logical_port);
		return link;
	}

	MapleDeviceRV readBlock(MapleLink& link, unsigned block)
	{
		if (cachedBlocks[block])
			return MDRS_JVSNone;

		MapleMsg txMsg;
		txMsg.command = MDCF_BlockRead;
		txMsg.originAP = inMsg->originAP;
		txMsg.destAP = inMsg->destAP;
		txMsg.pushData(MFID_1_Storage);
		txMsg.pushData<u32>(block << 24); // (BE) partition #, phase, block #
		MapleMsg rxMsg;
		if (link.sendReceive(txMsg, rxMsg) && rxMsg.size == 130)
		{
			DEBUG_LOG(MAPLE, "DreamPotatoVmu[%s]: read block %d", logical_port, block);
			memcpy(&flash_data[block * 4 * 128], &rxMsg.data[8], 4 * 128);
			cachedBlocks[block] = true;
			storageLinked = true;
		}
		else {
			ERROR_LOG(MAPLE, "Failed to read VMU %s: I/O error", logical_port);
			return MDRE_FileError; // I/O error
		}
		return MDRS_JVSNone;
	}

	u32 dma(u32 cmd) override
	{
		auto link = getMapleLink();
		if (!link || !link->storageEnabled())
			return MapleLinkVmu::dma(link, cmd);

		if (!link->isConnected())
			return MDRS_JVSNone;

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
					MapleMsg rxMsg;
					if (!link->sendReceive(*inMsg, rxMsg)) {
						ERROR_LOG(MAPLE, "Failed to write VMU %s: I/O error", logical_port);
						return MDRE_FileError;
					}
					storageLinked = true;
					if (rxMsg.command != MDRS_DeviceReply)
						return rxMsg.command;
					cachedBlocks[inMsg->data[7]] = true;
					DEBUG_LOG(MAPLE, "DreamPotatoVmu[%s]: write block %d", logical_port, inMsg->data[7]);
					break;
				}

				case MDCF_BlockRead:
				{
					u8 block = inMsg->data[7];
					MapleDeviceRV rc = readBlock(link.value(), block);
					if (rc != MDRS_JVSNone)
						return rc;
					break;
				}

				case MDCF_GetMediaInfo:
					// block 255 contains the media info
					readBlock(link.value(), 255);
					break;

				default:
					// do nothing
					break;
				}
			}
		}
		return MapleLinkVmu::dma(cmd);
	}

	bool linkStatus() override
	{
		auto link = MapleLinkRegistry::GetMapleLink(bus_id, bus_port);
		if (!link)
			return false;
		if (link->storageEnabled())
			return link->isConnected();
		else
			return true; // local storage means this must always remain "linked"
	}
};

class DreamPotato : public BaseDreamLink
{
	int bus;
	asio::ip::tcp::iostream iostream;
	bool connected = false;
	static constexpr u16 BASE_PORT = 37393;
	time_t lastMsg = 0;
	u32 initedPorts = 0;

public:
	DreamPotato(int bus)
		: BaseDreamLink(true), bus(bus)
	{}

	bool send(const MapleMsg& msg) override;
	bool sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg) override;

	bool isConnected() override {
		checkConnection();
		return connected;
	}
	const char* getName() const override {
		return "DreamPotato";
	}

	std::shared_ptr<maple_device> createMapleDevice(int bus, int port) override {
		return std::make_shared<DreamPotatoVmu>();
	}

	void connect();
	void disconnect();
	void init(u32 ports);

	void checkConnection()
	{
		if (!connected)
			return;
		time_t now = getTimeMs();
		if (now - lastMsg < 1000)
			return;

		lastMsg = now;
		asio::ip::tcp::socket& sock = static_cast<asio::ip::tcp::socket&>(iostream.socket());
		bool nonBlocking = sock.non_blocking();
		std::error_code ec;
		if (!nonBlocking)
			sock.non_blocking(true, ec);
		// We don't expect to have anything to read
		u8 buf;
		size_t read = sock.read_some(asio::buffer(&buf, 1), ec);
		if (!nonBlocking) {
			std::error_code ignored;
			sock.non_blocking(false, ignored);
		}
		if (read != 1 && !!ec && ec != asio::error::would_block)
		{
			connected = false;
			asyncRetryConnect();
		}
	}
};

static std::array<std::shared_ptr<DreamPotato>, 4> Potatoes;

void DreamPotato::init(u32 ports)
{
	if (ports != initedPorts)
	{
		registerLink(bus, ports);
		initedPorts = ports;
	}
}

void DreamPotato::connect()
{
	if (connected)
		return;
	iostream = asio::ip::tcp::iostream("::1", std::to_string(BASE_PORT + bus), asio::ip::resolver_base::flags::numeric_host);
	if (!iostream)
		iostream = asio::ip::tcp::iostream("127.0.0.1", std::to_string(BASE_PORT + bus), asio::ip::resolver_base::flags::numeric_host);
	if (!iostream) {
		WARN_LOG(INPUT, "DreamPotato[%d] connection failed: %s", bus, iostream.error().message().c_str());
		disconnect();
		return;
	}
	connected = true;
	iostream.expires_from_now(std::chrono::milliseconds(500));

	MapleMsg txMsg;
	txMsg.command = MDC_DeviceRequest;
	txMsg.destAP = (bus << 6) | 1;
	txMsg.originAP = bus << 6;
	MapleMsg rxMsg;
	// Note: connected *must* be true or infinite recursion loop
	if (!sendReceive(txMsg, rxMsg))
		// error has been logged and disconnect() called
		return;
	const u32 fnCode = rxMsg.readData<u32>();
	if ((fnCode & MFID_1_Storage) == 0)
	{
		WARN_LOG(INPUT, "Unrecognized response from DreamPotato");
		disconnect();
		return;
	}

	NOTICE_LOG(INPUT, "Connected to DreamPotato[%d]", bus);
}

void DreamPotato::disconnect()
{
	if (!connected)
		return;
	if (storageEnabled())
		disableStorage();
	connected = false;
	iostream.close();
	NOTICE_LOG(INPUT, "Disconnected from DreamPotato[%d]", bus);
}

bool DreamPotato::send(const MapleMsg& msg)
{
	if (!connected)
		return false;

	iostream.expires_from_now(std::chrono::milliseconds(100));
	asio::error_code ec = sendMsg(msg, iostream);
	if (ec)
	{
		WARN_LOG(INPUT, "DreamPotato[%d] send failed: %s", bus, ec.message().c_str());
		connected = false;
		asyncRetryConnect();
		return false;
	}
	lastMsg = getTimeMs();
	return true;
}

bool DreamPotato::sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg)
{
	if (!send(txMsg))
		return false;

	if (!receiveMsg(rxMsg, iostream)) {
		WARN_LOG(INPUT, "DreamPotato[%d] receive failed", bus);
		connected = false;
		asyncRetryConnect();
		return false;
	}
	return true;
}

// Instantiate DreamPotato devices where needed and delete the others
void update()
{
	for (unsigned bus = 0; bus < Potatoes.size(); bus++)
	{
		auto& potato = Potatoes[bus];

		const bool inPort1 = (
			config::NetworkExpansionDevices[bus][0] == 1 &&
			config::MapleExpansionDevices[bus][0] == MDT_SegaVMU
		);
		const bool inPort2 = (
			config::NetworkExpansionDevices[bus][1] == 1 &&
			config::MapleExpansionDevices[bus][1] == MDT_SegaVMU
		);
		u32 ports = (inPort1 ? 0x01 : 0) | (inPort2 ? 0x02 : 0);

		if (
			(inPort1 && maple_getPortCount(config::MapleMainDevices[bus]) >= 1) ||
			(inPort2 && maple_getPortCount(config::MapleMainDevices[bus]) >= 2)
		)
		{
			if (potato == nullptr) {
				potato = std::make_shared<DreamPotato>(bus);
			}

			potato->init(ports);
		}
		else if (potato != nullptr) {
			potato->term();
			potato = nullptr;
		}
	}
}

void term()
{
	for (unsigned bus = 0; bus < Potatoes.size(); bus++)
	{
		auto& potato = Potatoes[bus];
		if (potato != nullptr) {
			potato->term();
			potato = nullptr;
		}
	}
}

} // namespace dreampotato
