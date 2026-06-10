/*
	This file is part of Flycast.
	Portions Copyright 2026 The Hollycast Authors

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
#include <filesystem>

namespace ip = asio::ip;
using tcp = ip::tcp;

//! Custom Maple message to instruct DreamPotato to open a file by path
static const u8 MDC_DPOpenFile = 0xC0;

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

static asio::error_code receiveMsg(MapleMsg& msg, tcp::socket& sock, asio::io_context& ioContext)
{
	asio::streambuf buf;
	asio::error_code read_ec = asio::error::would_block;
	asio::async_read_until(sock, buf, "\r\n",
		[&read_ec](const asio::error_code& ec, size_t n) {
			read_ec = ec;
		});

	// Run handlers until either deadline passes or our handler is completed (which updates 'read_ec')
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	ioContext.restart();
	while (read_ec == asio::error::would_block) {
		if (ioContext.run_one_until(deadline) == 0)
			break;
	}

	if (read_ec == asio::error::would_block)
	{
		// Timeout. Cancel the ongoing socket operation and drain the
		// ioContext queue so that the callback runs while 'buf' is still live
		sock.cancel();
		ioContext.poll();
		return read_ec;
	}

	if (read_ec)
		return read_ec;

	std::string response;
	std::istream istream(&buf);
	std::getline(istream, response);
	sscanf(response.c_str(), "%hhx %hhx %hhx %hhx", &msg.command, &msg.destAP, &msg.originAP, &msg.size);
	if ((msg.getDataSize() - 1) * 3 + 13 >= response.length())
		return asio::error::message_size;

	for (unsigned i = 0; i < msg.getDataSize(); i++)
		sscanf(&response[i * 3 + 12], "%hhx", &msg.data[i]);
	return read_ec;
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

#ifdef DREAMPOTATO_INTEGRATED_MODE
	void OnSetup() override;

	bool fullSave() override
	{
		// We want fullSaves to occur when integrated mode is used, so that
		// the VMU file is ready on disk to pass off to DreamPotato.
		if (config::DreamPotatoIntegratedMode || !link.storageEnabled())
		{
			return maple_sega_vmu::fullSave();
		}

		// Skip virtual save when using MapleLink VMU
		DEBUG_LOG(MAPLE, "Full save ignored for MapleLink VMU");
		return true;
	}
#endif

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

	void requestReconnect() override
	{
		if (link.storageEnabled())
		{
			accessed_blocks_valid = true;
			memset(&flash_data[0], 0, sizeof(flash_data));
			memset(&accessed_blocks[0], 0, sizeof(accessed_blocks));
		}

		maple_base::requestReconnect();
	}
};

class DreamPotato : public BaseDreamLink
{
	int bus;
	asio::io_context ioContext;
	tcp::socket sock{ioContext};

	enum class ConnectionStatus { Disconnected, Connecting, Connected };

	ConnectionStatus connectionStatus = ConnectionStatus::Disconnected;
	static constexpr u16 BASE_PORT = 37393;
	time_t lastMsg = 0;
	u32 initedPorts = 0;

#ifdef DREAMPOTATO_INTEGRATED_MODE
	//! The handle of the child DreamPotato process. Only used in integrated mode.
	os_Process processHandle;
	//! Acceptor for DreamPotato to connect to. Only used in integrated mode.
	tcp::acceptor acceptor{ioContext};
#endif

	//! Ports which contain a docked VMU in DreamPotato
	u32 dockedPorts = 0;
	//! Ports whose devices need to be reconnected due to an external state change
	u32 needsReconnectPorts = 0;

public:
	DreamPotato(int bus)
		: BaseDreamLink(true), bus(bus)
	{}

	bool send(const MapleMsg& msg) override;
	bool sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg) override;

#ifdef DREAMPOTATO_INTEGRATED_MODE
	bool storageEnabled() override {
		return config::DreamPotatoIntegratedMode || BaseDreamLink::storageEnabled();
	}
#endif

	bool isConnected() override {
		checkConnection();
		return isConnectedInternal();
	}

	//! Does not perform side effects such as completing a pending connection.
	bool isConnectedInternal() const {
		return connectionStatus == ConnectionStatus::Connected;
	}

	const char* getName() const override {
		return "DreamPotato";
	}

	u32 getFunctionCodesMask(int forPort) const override {
		return isPortDocked(forPort) ? (MFID_1_Storage | MFID_2_LCD | MFID_3_Clock) : 0;
	}

	std::shared_ptr<maple_device> createMapleDevice(int bus, int port) override {
		return std::make_shared<DreamPotatoVmu>(MapleLink(shared_from_this(), bus, port));
	}

	void connect() override;
	void connectComplete(const asio::error_code& ec);
	void disconnect() override;
	void init(u32 ports);
	void term() override;

	bool isPortDocked(int port) const
	{
		assert(port == 0 || port == 1);
		return dockedPorts & (1 << port);
	}

	bool portNeedsReconnect(int port)
	{
		assert(port == 0 || port == 1);
		int portFlag = 1 << port;
		bool needsReconnect = portFlag & needsReconnectPorts;
		needsReconnectPorts &= ~portFlag;
		return needsReconnect;
	}

#ifdef DREAMPOTATO_INTEGRATED_MODE
	void startAccepting()
	{
		acceptor.async_accept(
			[this](const asio::error_code& ec, tcp::socket newSock) {
				if (connectionStatus == ConnectionStatus::Connected) {
					// Client is already connected. Drop the new connection.
					newSock.close();
				} else {
					assert(!sock.is_open());
					sock = std::move(newSock);
					connectComplete(ec);
				}

				// Listen for another client
				startAccepting();
			});
	}

	void sendFilePath(int port)
	{
		assert(port == 0 || port == 1);
		MapleMsg msg;
		msg.command = MDC_DPOpenFile;
		msg.destAP = (bus << 6) | (1 << port);
		msg.originAP = bus << 6;
		msg.pushData(MFID_1_Storage);

		std::string logical_port;
		logical_port.push_back('A' + bus);
		logical_port.push_back('1' + port);

		// Only send the file path if a preceding 'OnSetup()' has placed a 128KB VMU file on disk.
		std::string path = hostfs::getVmuPath(logical_port, /*save*/ false);
		std::error_code ec;
		size_t fileSize = std::filesystem::file_size(path, ec);
		if (ec) {
			WARN_LOG(INPUT, "DreamPotato[%d] VMU file '%s' error: %s", bus, path.c_str(), ec.message().c_str());
			return;
		}
		else if (fileSize != sizeof(maple_sega_vmu::flash_data)) {
			WARN_LOG(INPUT, "DreamPotato[%d] VMU file '%s' unexpected size: %ld", bus, path.c_str(), fileSize);
			return;
		}

		// Null-pad the string, so that trailing garbage does not get sent over the wire.
		int remainder = path.length() % 4;
		if (remainder != 0) {
			while (remainder < 4) {
				path.push_back('\0');
				remainder++;
			}
		}

		msg.pushData(path.c_str(), path.size());
		ec = sendMsg(msg, sock);
		if (ec) {
			WARN_LOG(INPUT, "DreamPotato[%d] sendFilePath error: %s", bus, ec.message().c_str());
			disconnect();
		}
	}
#endif

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

		ec = receiveMsg(msg, sock, ioContext);
		if (ec == asio::error::message_size) {
			// HACK: Here we just received a refresh message. Retry 'receiveMsg()' once here to avoid a clunky disconnect+reconnect.
			// DP is sending a refresh message because it thinks it needs to tell us about a change in the docked state or the currently loaded VMU file.
			// But here, we're already in the process of querying the expansion devices. We don't need to be told to do that.
			// See also TODO in 'checkConnection()'--we want to do away with refresh messages and use a better solution like polling instead
			ec = receiveMsg(msg, sock, ioContext);
			DEBUG_LOG(INPUT, "DreamPotato[%d] ignoring a spurious refresh message", bus);
		}

		if (ec) {
			WARN_LOG(INPUT, "DreamPotato[%d] updateExpansionDevs error: %s", bus, ec.message().c_str());
			disconnect();
		}

		dockedPorts = msg.originAP & 0x1f;
		needsReconnectPorts = dockedPorts;
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

		// Check for a refresh message, which indicates we should re-query which expansion slots have VMUs in them
		// TODO: We should get rid of this and 'isSocketDisconnected()' and poll 'MDCF_GetCondition' instead.
		// However, corresponding changes in DreamPotato will also be needed for us to be able to detect when external state has changed.
		constexpr std::string_view refreshMessage = "FF FF FF FF\r\n";
		constexpr int refreshMessageSize = refreshMessage.length();
		asio::error_code ec;
		if (sock.available(ec) >= refreshMessageSize)
		{
			std::array<char, refreshMessageSize> data;
			int bytesPeeked = recv(sock.native_handle(), data.data(), data.size(), MSG_PEEK);

			MapleMsg message;
			sscanf(data.data(), "%hhx %hhx %hhx %hhx", &message.command, &message.destAP, &message.originAP, &message.size);
			if (message.command == 0xff && message.destAP == 0xff && message.originAP == 0xff && message.size == 0xff) {
				// It is a refresh message, so consume it.
				receiveMsg(message, sock, ioContext);
				updateExpansionDevs();
			}
		}

		// Detect a graceful TCP disconnect.
		if (isSocketDisconnected())
		{
			disconnect();
			return;
		}

		if (connectionStatus == ConnectionStatus::Disconnected)
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
	auto* potato = dynamic_cast<DreamPotato*>(link.dreamlink.get());
	if (potato->portNeedsReconnect(link.port))
		requestReconnect();
	if (!MapleLinkVmu::linkStatus())
		return false;

	if (link.storageEnabled())
		return link.isConnected() && potato->isPortDocked(link.port);
	else
		return true; // local storage means this must always remain "linked"
}

#ifdef DREAMPOTATO_INTEGRATED_MODE
void DreamPotatoVmu::OnSetup()
{
	if (!config::DreamPotatoIntegratedMode) {
		MapleLinkVmu::OnSetup();
		return;
	}

	// Setup the backing VMU file on disk, then close it so we can hand control of it over to DreamPotato.
	maple_sega_vmu::OnSetup();
	if (file != nullptr) {
		std::fclose(file);
		file = nullptr;
	}

	auto* potato = dynamic_cast<DreamPotato*>(link.dreamlink.get());
	if (potato->isConnectedInternal()) {
		potato->sendFilePath(bus_port);
	}
}
#endif

static std::array<std::shared_ptr<DreamPotato>, 4> Potatoes;

void DreamPotato::init(u32 ports)
{
	if (ports != initedPorts)
	{
		registerLink(bus, ports);
		initedPorts = ports;

#ifdef DREAMPOTATO_INTEGRATED_MODE
		// Close DreamPotato so we can reopen it with the new ports below
		if (config::DreamPotatoIntegratedMode)
			processHandle.terminate();
#endif
	}

#ifdef DREAMPOTATO_INTEGRATED_MODE
	if (config::DreamPotatoIntegratedMode && !processHandle.isRunning())
	{
		std::string dreamPotatoPath = hostfs::getDreamPotatoPath();
		if (dreamPotatoPath.empty()) {
			WARN_LOG(INPUT, "Could not launch DreamPotato because the executable was not found");
			return;
		}

		if (!acceptor.is_open()) {
			// Let the OS automatically assign a port to the acceptor
			tcp::endpoint ep(ip::address_v6::loopback(), 0);
			acceptor.open(ep.protocol());
			acceptor.bind(ep);
			acceptor.listen(1);
			startAccepting();
		}

		// TODO: should we call 'disconnect()' or anything here?
		// Should we assert any expectation about the 'connectionStatus'?

		u16 tcpPort = acceptor.local_endpoint().port();
		std::string tcpPortString = std::to_string(tcpPort);

		// Note: Hollycast uses "bus+port" to refer to "A1", for example,
		// while DreamPotato uses "port+slot".
		std::string portString(1, 'A' + bus);
		std::string slotsString = ports == 3 ? "both"
			: ports == 1 ? "1"
			: ports == 2 ? "2"
			: "?";

		INFO_LOG(INPUT, "DreamPotato[%s]: starting subprocess with TCP port %s", portString.c_str(), tcpPortString.c_str());
		processHandle = os_Process::start(dreamPotatoPath, {
			"--integrated",
			"--tcp-port",
			tcpPortString,
			"--slots",
			slotsString,
			"--port",
			portString
		});
	}
#endif
}

void DreamPotato::term()
{
	BaseDreamLink::term();
#ifdef DREAMPOTATO_INTEGRATED_MODE
	acceptor.close();
	processHandle.terminate();
	assert(!processHandle.isValid());
#endif
}

void DreamPotato::connect()
{
	if (connectionStatus != ConnectionStatus::Disconnected)
		return;

#ifdef DREAMPOTATO_INTEGRATED_MODE
	if (config::DreamPotatoIntegratedMode) {
		// connect() is a no-op in integrated mode, because we are waiting for DreamPotato to connect to us instead
		// TODO: perhaps we should spawn DreamPotato again here, if it is not currently running.
		// However, we need to take care to not cause "crash loops" of DreamPotato processes on the user's machine.
		DEBUG_LOG(INPUT, "DreamPotato[%d]: Ignoring DreamPotato::connect() call in integrated mode", bus);
		return;
	}
#endif

	connectionStatus = ConnectionStatus::Connecting;
	sock = tcp::socket{ioContext};
	sock.async_connect(tcp::endpoint(ip::address_v6::loopback(), BASE_PORT + bus),
		[this](const asio::error_code& ec) { connectComplete(ec); });
}

void DreamPotato::connectComplete(const asio::error_code& ec)
{
	if (ec) {
		WARN_LOG(INPUT, "DreamPotato[%d] connection failed: %s", bus, ec.message().c_str());
		connectionStatus = ConnectionStatus::Disconnected;
		return;
	}

	connectionStatus = ConnectionStatus::Connected;

#ifdef DREAMPOTATO_INTEGRATED_MODE
	if (config::DreamPotatoIntegratedMode) {
		if (initedPorts & (1 << 0))
			sendFilePath(0);

		if (initedPorts & (1 << 1))
			sendFilePath(1);
	}
#endif

	updateExpansionDevs();
	NOTICE_LOG(INPUT, "Connected to DreamPotato[%d]", bus);
}

void DreamPotato::disconnect()
{
	if (connectionStatus == ConnectionStatus::Disconnected)
		return;

	// Note: when we disconnect in integrated mode, we can't retry the connection from our side.
	// Instead, we expect that DreamPotato will retry the connection, and we will just passively wait for that to occur.
	connectionStatus = ConnectionStatus::Disconnected;
	dockedPorts = 0;

	asio::error_code ec;
	sock.close(ec);
	if (ec)
		WARN_LOG(INPUT, "DreamPotato socket close error: %s", ec.message().c_str());

	NOTICE_LOG(INPUT, "Disconnected from DreamPotato[%d]", bus);
}

bool DreamPotato::send(const MapleMsg& msg)
{
	if (connectionStatus != ConnectionStatus::Connected)
		return false;

	asio::error_code ec = sendMsg(msg, sock);
	if (ec)
	{
		WARN_LOG(INPUT, "DreamPotato[%d] send failed: %s", bus, ec.message().c_str());
		connectionStatus = ConnectionStatus::Disconnected;
		return false;
	}
	lastMsg = getTimeMs();
	return true;
}

bool DreamPotato::sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg)
{
	if (!send(txMsg))
		return false;

	if (auto ec = receiveMsg(rxMsg, sock, ioContext)) {
		WARN_LOG(INPUT, "DreamPotato[%d] receive error: %s", bus, ec.message().c_str());
		connectionStatus = ConnectionStatus::Disconnected;
		return false;
	}
	return true;
}

// Instantiate DreamPotato devices where needed and delete the others
void update()
{
#ifdef DREAMPOTATO_INTEGRATED_MODE
	static bool integratedMode = config::DreamPotatoIntegratedMode;
	if (integratedMode != config::DreamPotatoIntegratedMode) {
		// Integrated mode setting changed--tear everything down and recreate
		integratedMode = config::DreamPotatoIntegratedMode;
		term();
	}
#endif

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
