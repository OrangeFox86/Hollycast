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

namespace ip = asio::ip;
using tcp = ip::tcp;

static asio::error_code sendMsg(const MapleMsg& msg, tcp::socket& sock)
{
	static char buffer[1024 * 3 + 2];
	char *p = buffer;

	p += sprintf(p, "%02X %02X %02X %02X", msg.command, msg.destAP, msg.originAP, msg.size);
	const u32 sz = msg.getDataSize();
	for (u32 i = 0; i < sz; i++)
		p += sprintf(p, " %02X", msg.data[i]);
	strcpy(p, "\r\n");
	p += 2;
	asio::error_code ec;
	// TODO: perhaps writes should also be async with timeout
	asio::write(sock, asio::buffer(buffer, p - buffer), ec);
	return ec;
}

static bool receiveMsg(MapleMsg& msg, tcp::socket& sock, asio::io_context& ioContext,
	std::chrono::milliseconds timeout = std::chrono::milliseconds(100))
{
	asio::streambuf buf;
	asio::error_code read_ec = asio::error::would_block;
	size_t read_n = 0;
	
	// TODO: it feels like 'asio::use_future' could be cleaner but don't have that working yet
	asio::async_read_until(sock, buf, "\r\n",
		[&read_ec, &read_n](const asio::error_code& ec, size_t n) {
			read_ec = ec;
			read_n = n;
		});

	ioContext.restart();
	ioContext.run_for(timeout);

	if (read_ec == asio::error::would_block)
	{
		sock.cancel();
		ioContext.run(); // drain pending i/o queue
		return false;
	}

	if (read_ec)
		return false;

	std::string response;
	std::istream istream(&buf);
	std::getline(istream, response);
	sscanf(response.c_str(), "%hhx %hhx %hhx %hhx", &msg.command, &msg.destAP, &msg.originAP, &msg.size);
	if ((msg.getDataSize() - 1) * 3 + 13 >= response.length())
		return false;
	for (unsigned i = 0; i < msg.getDataSize(); i++)
		sscanf(&response[i * 3 + 12], "%hhx", &msg.data[i]);
	return true;
}


namespace dreampotato
{

//! A specialized VMU which interfaces with a MapleLink's VMU, including read/write operations
struct DreamPotatoVmu : public MapleLinkVmu
{
	bool userNotified = false;
	u32 fnCode = 0;

	DreamPotatoVmu(const MapleLink& link) : MapleLinkVmu(link)
	{}

	void deserialize(Deserializer& deser) override
	{
		deserializeVmu(deser, link.storageEnabled());
	}

	MapleDeviceRV readBlock(unsigned block)
	{
		if (accessed_blocks[block])
			return MDRS_JVSNone;

		std::optional<MapleMsg> rxMsg = sendRead(inMsg->destAP, inMsg->originAP, block);
		if (rxMsg.has_value() && rxMsg->size == 130)
		{
			DEBUG_LOG(MAPLE, "DreamPotatoVmu[%s]: read block %d", logical_port, block);
			memcpy(&flash_data[block * 4 * 128], &rxMsg->data[8], 4 * 128);
			accessed_blocks[block] = true;
		}
		else {
			ERROR_LOG(MAPLE, "Failed to read VMU %s: I/O error", logical_port);
			return MDRE_FileError; // I/O error
		}
		return MDRS_JVSNone;
	}

	u32 dma(u32 cmd) override
	{
		if (!link.storageEnabled())
			return MapleLinkVmu::dma(cmd);

		if (!link.isConnected())
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
					if (!link.sendReceive(*inMsg, rxMsg)) {
						ERROR_LOG(MAPLE, "Failed to write VMU %s: I/O error", logical_port);
						return MDRE_FileError;
					}
					if (rxMsg.command != MDRS_DeviceReply)
						return rxMsg.command;
					accessed_blocks[inMsg->data[7]] = true;
					DEBUG_LOG(MAPLE, "DreamPotatoVmu[%s]: write block %d", logical_port, inMsg->data[7]);
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

				default:
					// do nothing
					break;
				}
			}
		}
		return MapleLinkVmu::dma(cmd);
	}

	void poll()
	{
		// TODO: check for changes in device configuration.
		// 'DreamPotato::checkConnection()' may be more appropriate place
		return;

		MapleMsg txMsg;
		txMsg.command = MDC_DeviceRequest;
		txMsg.destAP = (link.bus << 6) | link.port;
		txMsg.originAP = link.bus << 6;
		MapleMsg rxMsg;
		// Note: connected *must* be true or infinite recursion loop
		if (!link.sendReceive(txMsg, rxMsg))
			// error has been logged and disconnect() called
			return;

		u32 newFnCode = rxMsg.readData<u32>();
		if (newFnCode != fnCode) {
			requestReconnect();
			fnCode = newFnCode;
		}
	}

	bool linkStatus() override
	{
		poll();

		if (!MapleLinkVmu::linkStatus())
			return false;

		if (link.storageEnabled())
			return link.isConnected();
		else
			return true; // local storage means this must always remain "linked"
	}
};

class DreamPotato : public BaseDreamLink
{
	int bus;
	asio::io_context ioContext;
	tcp::socket sock{ioContext};

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
		return std::make_shared<DreamPotatoVmu>(MapleLink(shared_from_this(), bus, port));
	}

	void connect() override;
	void connectComplete(const asio::error_code& ec);
	void disconnect() override;
	void init(u32 ports);

	void checkConnection()
	{
		// Complete connection if needed
		ioContext.poll();

		time_t now = getTimeMs();
		if (now - lastMsg < 1000)
			return;

		// Handle periodic tasks
		lastMsg = now;
		if (!connected)
		{
			// Been >=1000ms since last connection attempt. Try again.
			connect();
			return;
		}

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
			connect();
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

	sock = tcp::socket{ioContext};
	sock.async_connect(tcp::endpoint(ip::address_v6::loopback(), BASE_PORT + bus),
		[this](const asio::error_code& ec) { connectComplete(ec); });
}

void DreamPotato::connectComplete(const asio::error_code& ec)
{
	if (ec) {
		WARN_LOG(INPUT, "DreamPotato[%d] connection failed: %s", bus, ec.message().c_str());
		return;
	}

	connected = true;
	NOTICE_LOG(INPUT, "Connected to DreamPotato[%d]", bus);
}

void DreamPotato::disconnect()
{
	if (!connected)
		return;

	connected = false;
	asio::error_code ec;
	sock.close(ec);
	if (ec)
		WARN_LOG(INPUT, "DreamPotato socket close error: %s", ec.message().c_str());

	NOTICE_LOG(INPUT, "Disconnected from DreamPotato[%d]", bus);
}

bool DreamPotato::send(const MapleMsg& msg)
{
	if (!connected)
		return false;

	asio::error_code ec = sendMsg(msg, sock);
	if (ec)
	{
		WARN_LOG(INPUT, "DreamPotato[%d] send failed: %s", bus, ec.message().c_str());
		connected = false;
		return false;
	}
	lastMsg = getTimeMs();
	return true;
}

bool DreamPotato::sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg)
{
	if (!send(txMsg))
		return false;

	if (!receiveMsg(rxMsg, sock, ioContext)) {
		WARN_LOG(INPUT, "DreamPotato[%d] receive failed", bus);
		connected = false;
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
