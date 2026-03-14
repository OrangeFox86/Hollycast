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
	asio::async_read_until(sock, buf, "\r\n",
		[&read_ec](const asio::error_code& ec, size_t n) {
			read_ec = ec;
		});

	ioContext.restart();
	ioContext.run_for(timeout);
	if (read_ec == asio::error::would_block)
	{
		// Timeout. Cancel the ongoing socket operation and drain the
		// ioContext queue so that the callback runs while 'buf' is still live
		sock.cancel();
		ioContext.poll();
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

	bool linkStatus() override;
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

	//! Ports which contain a docked VMU in DreamPotato
	u32 dockedPorts = 0;

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

	bool isPortDocked(int port)
	{
		return dockedPorts & port;
	}

	void updateExpansionDevs()
	{
		// Check which expansion slots contain VMUs
		MapleMsg msg;
		msg.command = MDCF_GetCondition;
		msg.destAP = (bus << 6) | 0x20;
		msg.originAP = bus << 6;
		msg.pushData(MFID_0_Input);

		auto ec = sendMsg(msg, sock);
		if (ec)
		{
			WARN_LOG(INPUT, "DreamPotato[%d] updateExpansionDevs error: %s", bus, ec.message().c_str());
			disconnect();
		}
		if (!receiveMsg(msg, sock, ioContext)) {
			WARN_LOG(INPUT, "DreamPotato[%d] read timeout", bus);
			disconnect();
		}

		dockedPorts = msg.originAP & 0x1f;
	}

	bool isSocketDisconnected() {
		// A socket was disconnected if 'select()' says the socket is ready to read, and a subsequent 'recv()' fails or says 0 bytes available to read.
		auto nativeHandle = sock.native_handle();
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(nativeHandle, &readfds);
		timeval timeout = { 0, 0 };
		// nfds should be set to the highest-numbered file descriptor plus 1.
		// See https://www.man7.org/linux/man-pages/man2/select.2.html
		int nfds = nativeHandle + 1;
		int nReady = select(nfds, &readfds, nullptr, nullptr, &timeout);
		bool socketIsReady = nReady > 0 && FD_ISSET(nativeHandle, &readfds);
		if (!socketIsReady)
			return false;

		char dest;
		int len = recv(nativeHandle, &dest, sizeof(dest), MSG_PEEK);
		return len <= 0;
	}

	void checkConnection()
	{
		// Possibly complete a pending connection.
		ioContext.poll();

		// TODO: we should probably poll the attached devices, to both get up-to-date info,
		// and effectively get a heartbeat for the connection,
		// instead of needing to both check for reset message and tcp disconnect.

		// Check for a refresh message, which indicates we should re-query which expansion slots have VMUs in them
		constexpr std::string_view refreshMessage = "FF FF FF FF\r\n";
		constexpr int resetMessageSize = refreshMessage.length();
		asio::error_code ec;
		if (sock.available(ec) >= resetMessageSize)
		{
			std::array<u8, resetMessageSize> data;
			sock.read_some(asio::buffer(data), ec);
			if (ec)
			{
				WARN_LOG(INPUT, "DreamPotato reset message error: %s", ec.message().c_str());
				return;
			}

			// TODO: associated MapleLinks should request a reconnect.
			// Tricky as multiple links might both call thru checkConnection() and thus need a reconnect.
			std::string line(data.begin(), data.end());
			if (line == refreshMessage)
				updateExpansionDevs();
		}

		// Detect a graceful TCP disconnect.
		if (isSocketDisconnected())
		{
			disconnect();
			return;
		}

		if (!connected)
		{
			time_t now = getTimeMs();
			if (now - lastMsg < 1000)
				return;

			lastMsg = now;
			// Been >=1000ms since last connection attempt. Try again.
			connect();
		}
	}
};

bool DreamPotatoVmu::linkStatus()
{
	if (!MapleLinkVmu::linkStatus() || !link.isConnected())
		return false;

	auto* potato = dynamic_cast<DreamPotato*>(link.dreamlink.get());
	return potato->isPortDocked(link.port + 1);

	// TODO: should disabling physical memory really cause the VMU to report as connected always?
	// May want to revert that. Using DreamPotato without physical memory is a rare case.
}

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
	updateExpansionDevs();
	NOTICE_LOG(INPUT, "Connected to DreamPotato[%d]", bus);
}

void DreamPotato::disconnect()
{
	if (!connected)
		return;

	connected = false;
	dockedPorts = 0;
	// TODO: accessedBlocks of associated MapleLinks should be cleared.

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
