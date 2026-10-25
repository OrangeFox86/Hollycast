/*
    Copyright 2026 The Hollycast Authors

    This file is part of Hollycast.

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

#include "dreampicoport.h"
#include "maplelink.h"
#include "maplelinkregistry.h"

#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "ui/gui.h"
#include "cfg/option.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include "log/Log.h"
#include "emulator.h"

#include "DreamPicoPortApi.hpp"

// C++ standard library
#include <iomanip>
#include <sstream>
#include <thread>
#include <list>
#include <vector>
#include <array>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <string_view>

#ifndef TARGET_UWP
#include <asio.hpp>
#endif

#if defined(__linux__) || (defined(__APPLE__) && defined(TARGET_OS_MAC))
#include <dirent.h>
#endif

#if defined(_WIN32) && !defined(TARGET_UWP)
#include <windows.h>
#include <setupapi.h>
#endif

//! A Sega VMU with an optional file back-end
struct DppVirtualVmu : public maple_sega_vmu
{
    //! When true, the VMU is backed by a file on the file system
    const bool fileBacked;

    //! Default constructor (deleted)
    DppVirtualVmu() = delete;

    //! Constructor
    //! @param[in] fileBacked When true, VMU memory will be backed by a file; false for volatile memory only
    //! @param[in] primaryDev The primary device to mirror data from
    DppVirtualVmu(bool fileBacked, maple_device* primaryDev) :
        fileBacked(fileBacked)
    {
        maple_port = primaryDev->maple_port;
        bus_port = primaryDev->bus_port;
        bus_id = primaryDev->bus_id;
        memcpy(&logical_port[0], &primaryDev->logical_port[0], sizeof(logical_port));
        player_num = primaryDev->player_num;
        config = primaryDev->config;
    }

    virtual ~DppVirtualVmu()
    {
        // Need to nullify the config here to avoid double delete
        config->ResetImage();
        config = nullptr;
    }

    //! Called when this device is setup for use
    void OnSetup() override
    {
        if (fileBacked)
        {
            // Just use base's setup procedure
            maple_sega_vmu::OnSetup();
            return;
        }

        // Nullify the file and zero out all data under maple_sega_vmu
        file = nullptr;
        initializeVmu(); // Start with a valid VMU memory state
        memset(lcd_data, 0, sizeof(lcd_data));
        accessed_blocks_valid = true;
        memset(accessed_blocks, 0, sizeof(accessed_blocks));
        last_write_tick = 0;
        loaded_us_since_write = std::numeric_limits<u64>::max();
        fullSaveNeeded = false;
    }

    //! MapleLink relay is disabled for this virtual VMU
    inline void relayMapleLink() override
    {}
};

//! Generates the port character from a given bus
//! @param[in] bus The target bus [0,3]
//! @return The associated port character
static char getPortCharForBus(int bus) {
    return ('A' + bus);
}

static std::string getBusDescription(int software_bus, int hardware_bus) {
    const char swPortChar = getPortCharForBus(software_bus);
    const char hwPortChar = getPortCharForBus(hardware_bus);
    if (swPortChar == hwPortChar) {
        return std::string(1, swPortChar);
    } else {
        return std::string(1, swPortChar) + "," + std::string(1, hwPortChar);
    }
}

//! Generically handles any MapleLink device when supported
struct DppMapleLinkDevice : public MapleLinkDeviceBase<maple_base>
{
    //! The supported functions mask for this device
    const u32 supportedFns;
    //! The linked DreamPicoPort
    std::weak_ptr<class DreamPicoPort> linkedDppWptr;
    //! The last time write was performed
    std::chrono::steady_clock::time_point lastWriteTime;
    //! Mutex serializing write operations
    std::mutex writeMutex;
    //! Virtual VMU used to display a virtual screen and optionally save to file
    std::unique_ptr<DppVirtualVmu> virtualVmu;
    //! The last returned value from get_device_type()
    MapleDeviceType serializingType = MDT_None;
    //! The device type currently deserializing for
    MapleDeviceType deserializingType = MDT_None;

    //! Magic number used on serialize()
    static constexpr const u8 vmuSerializeMagic[4] = {0x04, 0x3d, 0x8b, 0xde};

    //! Constructor
    //! @param[in] supportedFns Supported functions mask (default: any function)
    DppMapleLinkDevice(const MapleLink& link, u32 supportedFns = std::numeric_limits<u32>::max());
    virtual ~DppMapleLinkDevice() = default;
    void OnSetup() override;
    bool linkStatus() override;
    void requestReconnect() override;
    MapleDeviceType get_device_type() override;
    static MapleDeviceType fnCodeToMapleDeviceType(u32 fnCodeMask);
    void establishVirtualDevice(MapleDeviceType dev);
    void establishVirtualVmu();
    u32 virtualVmuDma(u32 cmd);
    u32 dma(u32 cmd) override;

    bool usingExternalStorage() const;

    bool deserializingFor(MapleDeviceType type) override;
    void serialize(Serializer& ser) const override;
    void deserialize(Deserializer& deser) override;
};

//! Generically handles a MapleLink main device (normally a controller)
struct MapleLinkMainDevice : public DppMapleLinkDevice
{
    //! Constructor
    //! Only input devices are currently supported here - even lightguns and DreamEye implement MFID_0_Input
    //! This should support everything except for keyboard & mouse
    MapleLinkMainDevice(const MapleLink& link);
    MapleDeviceType get_device_type() override;
    u32 dma(u32 cmd) override;
};

class ApiDreamPicoPortComms
{
    //! All known dpp_api devices by serial number; already connected if set
    static std::unordered_map<std::string, std::weak_ptr<dpp_api::DppDevice>> all_dpp_api_devices;
    //! The mutex serializing access to all_dpp_api_devices
    static std::mutex all_dpp_api_devices_mutex;

    //! This is set when the device supports this new API
    std::shared_ptr<dpp_api::DppDevice> dpp_api_device;
    //! The bus ID dictated by flycast
    int software_bus = -1;
    //! The bus index of the hardware connection which will differ from the software bus
    const int hardware_bus = -1;
    //! Set to true when upgrade is required to continue
    bool upgrade_required = false;

public:
    ApiDreamPicoPortComms() = delete;

    ApiDreamPicoPortComms(const DreamPicoPort::HardwareInfo& hw_info, int software_bus) :
        software_bus(software_bus),
        hardware_bus(hw_info.hardware_bus)
    {
        std::lock_guard<std::mutex> lock(all_dpp_api_devices_mutex);

        auto iter = all_dpp_api_devices.find(hw_info.serial_number);
        if (iter != all_dpp_api_devices.end()) {
            dpp_api_device = iter->second.lock();
            if (!dpp_api_device) {
                // The weak pointer was no longer valid; remove item from map
                all_dpp_api_devices.erase(iter);
            }
        }

        if (!dpp_api_device) {
            dpp_api_device = makeNewDppDevice(hw_info);
            if (!dpp_api_device) {
                return; // Failed to make the new device
            }

            // Save this instance to the map
            all_dpp_api_devices.insert(std::make_pair(hw_info.serial_number, dpp_api_device));
        }
        else if (!dpp_api_device->isConnected()) {
            // Note: it is possible to reach here if DreamPicoPort is connected, removed from USB, and then reattached
            //       all while the emulator is paused.
            if (!dpp_api_device->connect()) {
                WARN_LOG(
                    INPUT,
                    "DreamPicoPort[%s] API reconnect failed: %s",
                    getLocDesc().c_str(),
                    dpp_api_device->getLastErrorStr().c_str()
                );
                dpp_api_device.reset();
            }
        }
    }

    virtual ~ApiDreamPicoPortComms() = default;

    void changeSoftwareBus(int software_bus) {
        this->software_bus = software_bus;
    }

    bool isConnected() const {
        return (dpp_api_device && dpp_api_device->isConnected());
    }

    bool isUpdateRequired() const {
        return upgrade_required;
    }

    bool initialize(std::chrono::milliseconds timeout_ms) {
        if(!isConnected()) {
            return false;
        }

        // SDL workaround to ensure axis values are up-to-date
        dpp_api_device->send(dpp_api::msg::tx::RefreshGamepad{static_cast<std::uint8_t>(hardware_bus)});

        return true;
    }

    std::array<dpp_api::GamepadConnectionState, 4> getConnectedGamepads() {
        return dpp_api_device->sendSync(dpp_api::msg::tx::GetConnectedGamepads{}).gamepadConnectionStates;
    }

    std::optional<std::vector<std::vector<std::array<uint32_t, 2>>>> getPeripherals(
        std::chrono::milliseconds timeout_ms
    ) {
        if (!isConnected()) {
            return std::nullopt;
        }

        std::vector<std::vector<std::array<uint32_t, 2>>> peripherals;

        dpp_api::msg::rx::GetDcSummary summary =
            dpp_api_device->sendSync(dpp_api::msg::tx::GetDcSummary{static_cast<uint8_t>(hardware_bus)});
        peripherals = summary.summary;

        return peripherals;
    }

    bool send(const MapleMsg& msg, std::chrono::milliseconds timeout_ms) {
        if (!isConnected()) {
            return false;
        }

        dpp_api::msg::tx::Maple tx;
        tx.emu = true;
        const u32 data_size = msg.getDataSize();
        tx.packet.reserve(data_size + 4);
        tx.packet.push_back(msg.command);
        // Need to message the hardware bus instead of the software bus
        u8 hwDestAP = (hardware_bus << 6) | (msg.destAP & 0x3F);
        u8 hwOriginAP = (hardware_bus << 6) | (msg.originAP & 0x3F);
        tx.packet.push_back(hwDestAP);
        tx.packet.push_back(hwOriginAP);
        tx.packet.push_back(msg.size);
        tx.packet.insert(tx.packet.end(), msg.data, msg.data + data_size);
        const uint64_t id = dpp_api_device->send(tx);
        return (id != 0);
    }

    bool send(const MapleMsg& txMsg, MapleMsg& rxMsg, std::chrono::milliseconds timeout_ms) {
        if (!isConnected()) {
            return false;
        }

        dpp_api::msg::tx::Maple tx;
        tx.emu = true;
        const u32 data_size = txMsg.getDataSize();
        tx.packet.reserve(data_size + 4);
        tx.packet.push_back(txMsg.command);
        // Need to message the hardware bus instead of the software bus
        u8 hwDestAP = (hardware_bus << 6) | (txMsg.destAP & 0x3F);
        u8 hwOriginAP = (hardware_bus << 6) | (txMsg.originAP & 0x3F);
        tx.packet.push_back(hwDestAP);
        tx.packet.push_back(hwOriginAP);
        tx.packet.push_back(txMsg.size);
        tx.packet.insert(tx.packet.end(), txMsg.data, txMsg.data + data_size);
        dpp_api::msg::rx::Maple rx = dpp_api_device->sendSync(tx, 100);
        if (rx.cmd != dpp_api::msg::rx::Msg::kCmdSuccess || rx.packet.size() < 4) {
            return false;
        }
        rxMsg.command = rx.packet[0];
        rxMsg.destAP = rx.packet[1];
        rxMsg.originAP = rx.packet[2];
        rxMsg.size = rx.packet[3];
        if (rx.packet.size() > 4) {
            memcpy(rxMsg.data, &rx.packet[4], (std::min)(rx.packet.size() - 4, sizeof(rxMsg.data)));
        }
        return (rxMsg.getDataSize() <= (rx.packet.size() - 4));
    }

    void sendPort(std::chrono::milliseconds timeout_ms) {
        dpp_api::msg::tx::ChangePlayerDisplay changePlayerDisplay;
        changePlayerDisplay.idx = hardware_bus;
        changePlayerDisplay.toIdx = software_bus;
        dpp_api_device->send(changePlayerDisplay, nullptr);
    }

    std::string getLocDesc(int software_bus = -1) const {
        if (software_bus < 0) {
            software_bus = this->software_bus;
        }
        return getBusDescription(software_bus, hardware_bus);
    }

private:
    std::shared_ptr<dpp_api::DppDevice> makeNewDppDevice(const DreamPicoPort::HardwareInfo& hw_info)
    {
        std::shared_ptr<dpp_api::DppDevice> newDev;

        if (hw_info.sys_dev >= 0)
        {
            newDev = dpp_api::DppDevice::open(hw_info.sys_dev);
        }
        else
        {
            dpp_api::DppDevice::Filter dppFilter;
            dppFilter.serial = hw_info.serial_number;
            dppFilter.minBcdDevice = 0;

            newDev = dpp_api::DppDevice::find(dppFilter);
        }

        if (!newDev) {
            WARN_LOG(
                INPUT,
                "DreamPicoPort[%s] API connect failed: find failed for serial %s",
                getLocDesc().c_str(),
                hw_info.serial_number.c_str()
            );

            return nullptr;
        }
        else if (newDev->getVersion() < std::array<std::uint8_t, 3>{1,2,1}) {
            upgrade_required = true;
            std::array<std::uint8_t, 3> ver = newDev->getVersion();
            WARN_LOG(
                INPUT,
                "DreamPicoPort[%s] API connect failed: device with serial \"%s\" uses version %i.%i.%i\n"
                "Update DreamPicoPort firmware to version 1.2.1 or later to enable peripheral connection",
                getLocDesc().c_str(),
                hw_info.serial_number.c_str(),
                static_cast<int>(ver[0]),
                static_cast<int>(ver[1]),
                static_cast<int>(ver[2])
            );

            return nullptr;
        }

        if (!newDev->connect()) {
            WARN_LOG(
                INPUT,
                "DreamPicoPort[%s] API connect failed: %s",
                getLocDesc().c_str(),
                newDev->getLastErrorStr().c_str()
            );

            return nullptr;
        }

        return newDev;
    }
};

std::unordered_map<std::string, std::weak_ptr<dpp_api::DppDevice>> ApiDreamPicoPortComms::all_dpp_api_devices;
std::mutex ApiDreamPicoPortComms::all_dpp_api_devices_mutex;


DppMapleLinkDevice::DppMapleLinkDevice(const MapleLink& link, u32 supportedFns) :
    MapleLinkDeviceBase<maple_base>(link), supportedFns(supportedFns)
{
    std::shared_ptr<DreamPicoPort> linkedDpp = std::dynamic_pointer_cast<DreamPicoPort>(link.dreamlink);
    linkedDppWptr = linkedDpp;
    if (!linkedDpp) {
        ERROR_LOG(INPUT, "DppMapleLinkDevice created without an associated DreamPicoPort");
    }
}

void DppMapleLinkDevice::OnSetup()
{
    // It doesn't make any sense for these to differ for DppMapleLinkDevices
    player_num = bus_id;

    maple_base::OnSetup();
}

bool DppMapleLinkDevice::linkStatus()
{
    if (!maple_base::linkStatus())
        return false;

    std::shared_ptr<DreamPicoPort> linkedDpp = linkedDppWptr.lock();

    bool isLinked = (
        linkedDpp &&
        linkedDpp->isConnected() &&
        ((linkedDpp->getFunctionCodesMask(bus_port) & supportedFns) != 0)
    );

    if (!isLinked) {
        virtualVmu.reset();
    }

    return isLinked;
}

void DppMapleLinkDevice::requestReconnect()
{
    if (virtualVmu)
    {
        // Reset the virtual VMU data
        virtualVmu->accessed_blocks_valid = true;
        memset(&virtualVmu->flash_data[0], 0, sizeof(virtualVmu->flash_data));
        memset(&virtualVmu->accessed_blocks[0], 0, sizeof(virtualVmu->accessed_blocks));
    }

    maple_base::requestReconnect();
}

MapleDeviceType DppMapleLinkDevice::get_device_type()
{
    // This is mainly used by the serializer
    serializingType = MDT_None;

    std::shared_ptr<DreamPicoPort> linkedDpp = linkedDppWptr.lock();

    if (!linkedDpp)
        return serializingType;

    serializingType = fnCodeToMapleDeviceType(linkedDpp->getFunctionCodesMask(bus_port));

    establishVirtualDevice(serializingType);

    return serializingType;
}

MapleDeviceType DppMapleLinkDevice::fnCodeToMapleDeviceType(u32 fnCodeMask)
{
    if (fnCodeMask & MFID_0_Input)
        return MDT_SegaController;
    else if (fnCodeMask & (MFID_1_Storage | MFID_2_LCD | MFID_3_Clock))
        return MDT_SegaVMU;
    else if (fnCodeMask & MFID_4_Mic)
        return MDT_Microphone;
    else if (fnCodeMask & (MFID_5_ARGun | MFID_7_LightGun))
        return MDT_LightGun;
    else if (fnCodeMask & MFID_6_Keyboard)
        return MDT_Keyboard;
    else if (fnCodeMask & MFID_8_Vibration)
        return MDT_PurupuruPack;
    else if (fnCodeMask & MFID_9_Mouse)
        return MDT_Mouse;
    else if (fnCodeMask & MFID_11_Camera)
        return MDT_Dreameye;

    return MDT_None;
}

void DppMapleLinkDevice::establishVirtualDevice(MapleDeviceType dev)
{
    switch (dev)
    {
        case MDT_SegaVMU:
            establishVirtualVmu();
            break;

        default:
            virtualVmu.reset();
            break;
    }
}

void DppMapleLinkDevice::establishVirtualVmu()
{
    if (!virtualVmu)
    {
        // Create the virtual VMU and have it share my data
        virtualVmu = std::make_unique<DppVirtualVmu>(!usingExternalStorage(), dynamic_cast<maple_device*>(this));
        virtualVmu->OnSetup();
    }
}

u32 DppMapleLinkDevice::virtualVmuDma(u32 cmd)
{
    establishVirtualVmu();

    return virtualVmu->Dma(
        cmd,
        reinterpret_cast<const u32*>(dma_buffer_in),
        dma_count_in,
        reinterpret_cast<u32*>(dma_buffer_out),
        *dma_count_out
    );
}

u32 DppMapleLinkDevice::dma(u32 cmd)
{
    std::shared_ptr<DreamPicoPort> linkedDpp = linkedDppWptr.lock();

    if (!linkedDpp)
        return MDRS_JVSNone;

    establishVirtualDevice(fnCodeToMapleDeviceType(linkedDpp->getFunctionCodesMask(bus_port)));

    // Deserialize the first data word without popping off of dma
    u32 firstWord = 0;
    if (inMsg->size >= 1) {
        firstWord = inMsg->readData<u32>(0);
    }

    bool isMemory = false;
    bool isMemoryRead = false;
    u8 memoryBlock = 0;

    if (
        firstWord == MFID_1_Storage &&
        (cmd == MDCF_GetMediaInfo || cmd == MDCF_BlockRead || cmd == MDCF_BlockWrite || cmd == MDCF_GetLastError)
    ) {
        if (!linkedDpp->storageEnabled()) {
            // Use virtual memory and return without accessing physical memory
            // This will use file-backed memory and automatically save when needed
            return virtualVmuDma(cmd);
        } else if (cmd == MDCF_BlockWrite) {
            // Send write to virtual memory and also continue below
            virtualVmuDma(cmd);
        } // else: continue below to read the data

        isMemoryRead = (cmd == MDCF_BlockRead);
        isMemory = (isMemoryRead || cmd == MDCF_BlockWrite);
        if (isMemory) {
            memoryBlock = inMsg->data[7];
        }
    } else if (cmd == MDCF_BlockWrite && firstWord == MFID_2_LCD) {
        // Send to virtual screen and also continue below
        virtualVmuDma(cmd);
    }

    std::unique_lock<std::mutex> lock(writeMutex, std::defer_lock);

    // If doing write operation, serialize operation and delay 10 ms between writes
    const bool isWriteOp = (cmd == MDCF_BlockWrite || cmd == MDCF_GetLastError);
    if (isWriteOp) {
        lock.lock();
        std::this_thread::sleep_until(lastWriteTime + std::chrono::milliseconds(10));
    }

    const MapleMsg& txMsg = *inMsg;
    MapleMsg rxMsg{};
    std::vector<u32> output;
    if (linkedDpp && linkedDpp->sendReceive(txMsg, rxMsg)) {
        // If this message came from a main peripheral, clear out attached flags (will be handled by base)
        if (rxMsg.originAP & 0x20) {
            rxMsg.originAP = (rxMsg.originAP & 0xE0);
        }

        for (u32 i = 0; i < rxMsg.getDataSize(); ++i) {
            w8(rxMsg.data[i]);
        }
    } else {
        rxMsg.command = MDRS_JVSNone;
    }

    // Save to accessed_blocks if this was a memory command
    if (isMemory) {
        bool success = false;
        if (isMemoryRead) {
            success = (rxMsg.size >= 130 && rxMsg.command == MDRS_DataTransfer);
            if (success) {
                // Mirror read data to the virtual VMU
                memcpy(&virtualVmu->flash_data[memoryBlock * 512], &rxMsg.data[8], 512);
            }
        } else {
            success = (rxMsg.command == MDRS_DeviceReply);
        }
        virtualVmu->accessed_blocks[memoryBlock] = success;
    }

    // If doing write operation, save time at this point
    if (isWriteOp) {
        lastWriteTime = std::chrono::steady_clock::now();
        lock.unlock();
    }

    return rxMsg.command;
}

bool DppMapleLinkDevice::usingExternalStorage() const
{
    return link.storageEnabled();
}

bool DppMapleLinkDevice::deserializingFor(MapleDeviceType type)
{
    deserializingType = type;
    bool isSameType = false;
    std::shared_ptr<DreamPicoPort> linkedDpp = linkedDppWptr.lock();

    if (linkedDpp) {
        MapleDeviceType detectedType = fnCodeToMapleDeviceType(linkedDpp->getFunctionCodesMask(bus_port));
        isSameType = (type != MDT_None && detectedType == type);
    }

    if (!isSameType) {
        os_notify(i18n::T("ATTENTION: Current hardware configuration changed since last save state"), 6000);
    }

    return isSameType;
}

void DppMapleLinkDevice::serialize(Serializer& ser) const
{
    // Assumption: the caller would have called get_device_type() just before serialize(), so serializingType should be
    //             set to the expected serialization type

    if (serializingType == MDT_None)
    {
        ERROR_LOG(INPUT, "DppMapleLinkDevice received serialize for MDT_None");
        return;
    }
    else if (serializingType >= MDT_Count)
    {
        ERROR_LOG(
            INPUT,
            "DppMapleLinkDevice received invalid type for serialize [%i]",
            static_cast<int>(serializingType)
        );
        return;
    }

    if (serializingType == MDT_SegaVMU && virtualVmu)
    {
        // Note: if (serializingType == MDT_SegaVMU) then virtualVmu will be set
        virtualVmu->serialize(ser);
    }
    else
    {
        // Serialize default state for the serializing type
        mcfg_SerializeDefaultDevice(ser, serializingType, bus_id, bus_port, player_num);
    }
}

void DppMapleLinkDevice::deserialize(Deserializer& deser)
{
    // Assumption: the caller would have called deserializingFor() just before deserialize(), so deserializingType
    //             should be set to the expected deserialization type

    std::shared_ptr<DreamPicoPort> linkedDpp;

    if (deserializingType == MDT_None)
    {
        ERROR_LOG(INPUT, "DppMapleLinkDevice received deserialize for MDT_None");
        requestReconnect();
        return;
    }
    else if (deserializingType >= MDT_Count)
    {
        ERROR_LOG(
            INPUT,
            "DppMapleLinkDevice received invalid type for deserialize [%i]",
            static_cast<int>(deserializingType)
        );
        requestReconnect();
        return;
    }
    else
    {
        linkedDpp = linkedDppWptr.lock();
        if (!linkedDpp)
        {
            ERROR_LOG(
                INPUT,
                "DppMapleLinkDevice no link setup to deserialize [%i]",
                static_cast<int>(deserializingType)
            );
            requestReconnect();
            return;
        }
    }

    MapleDeviceType detectedType = fnCodeToMapleDeviceType(linkedDpp->getFunctionCodesMask(bus_port));

    if (detectedType != deserializingType)
    {
        // Should never reach here
        ERROR_LOG(
            INPUT,
            "DppMapleLinkDevice::deserialize received non-matching type [%i]; detected type: [%i]",
            static_cast<int>(deserializingType),
            static_cast<int>(detectedType)
        );
        requestReconnect();
    }

    if (deserializingType == MDT_SegaVMU && detectedType == deserializingType)
    {
        establishVirtualVmu();
        deserializeVmu(deser, *virtualVmu, usingExternalStorage());
    }
    else
    {
        // Just throw out the data
        mcfg_DeserializeDiscardDevice(deser, deserializingType, bus_id, bus_port, player_num);
    }

    // Done deserializing - reset type
    deserializingType = MDT_None;
}


MapleLinkMainDevice::MapleLinkMainDevice(const MapleLink& link) : DppMapleLinkDevice(link, MFID_0_Input) {}

MapleDeviceType MapleLinkMainDevice::get_device_type()
{
    // This is mainly used by the serializer
    if (!linkStatus()) {
        serializingType = MDT_None;
    } else {
        serializingType = MDT_SegaController;
    }

    return serializingType;
}

u32 MapleLinkMainDevice::dma(u32 cmd)
{
    // Since MDCF_GetCondition gets called very often and is very well defined, handle it here
    if (cmd == MDCF_GetCondition)
    {
        std::vector<u32> output;

        PlainJoystickState pjs;
        config->GetInput(&pjs);

        std::shared_ptr<DreamPicoPort> linkedDpp = linkedDppWptr.lock();

        if (!linkedDpp || !linkedDpp->isConnected())
        {
            // Not connected
            return MDRS_JVSNone;
        }
        else
        {
            // Function definition is analog/button mask
            // byte 0: 0  0  0  0  0  0  0  0
            // byte 1: 0  0  a5 a4 a3 a2 a1 a0
            // byte 2: R2 L2 D2 U2 D  X  Y  Z
            // byte 3: R  L  D  U  St A  B  C
            const u32 fnDef = linkedDpp->getFunctionDefinitions(bus_port)[0]; // MFID_0_Input def is always at [0]

            // Function
            w32(MFID_0_Input);

            // state data
            // 2 key code
            w16(pjs.kcode | ~(((fnDef >> 8) & 0xFF00) | ((fnDef >> 24) & 0xFF))); // 0==pressed

            // analog axes
            w8(((fnDef & 0x0100) != 0) ? pjs.trigger[PJTI_R] : 0x80);
            w8(((fnDef & 0x0200) != 0) ? pjs.trigger[PJTI_L] : 0x80);
            w8(((fnDef & 0x0400) != 0) ? pjs.joy[PJAI_X1] : 0x80);
            w8(((fnDef & 0x0800) != 0) ? pjs.joy[PJAI_Y1] : 0x80);
            w8(((fnDef & 0x1000) != 0) ? pjs.joy[PJAI_X2] : 0x80);
            w8(((fnDef & 0x2000) != 0) ? pjs.joy[PJAI_Y2] : 0x80);

            return MDRS_DataTransfer;
        }
    }

    return DppMapleLinkDevice::dma(cmd);
}


//
// HardwareInfo Definitions
//

char DreamPicoPort::HardwareInfo::getPortChar() const {
    return getPortCharForBus(hardware_bus);
}

const char* DreamPicoPort::HardwareInfo::getProductName() {
    return "DreamPicoPort";
}

std::string DreamPicoPort::HardwareInfo::getName(const std::string& separator, bool forceBusLetter) const {
    std::string name = getProductName();
    if (!is_single_device || forceBusLetter) {
        name += separator + std::string(1, getPortChar());
    }
    return name;
}

//
// DreamPicoPort Definitions
//

DreamPicoPort::DreamPicoPort(int bus, HardwareInfo hw_info) :
    GamepadDreamLink(true),
    software_bus(bus),
    hw_info(hw_info),
    device_name(hw_info.getName())
{
}

// Need to define destructor here because of forward declaration of ApiDreamPicoPortComms
DreamPicoPort::~DreamPicoPort()
{
}

bool DreamPicoPort::send(const MapleMsg& msg)  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (!dpp_comms) {
        return false;
    }

    return dpp_comms->send(msg, timeout_ms);
}

bool DreamPicoPort::sendReceive(const MapleMsg& txMsg, MapleMsg& rxMsg)  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (!dpp_comms) {
        return false;
    }

    return dpp_comms->send(txMsg, rxMsg, timeout_ms);
}

void DreamPicoPort::sendGameId(int expansion) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (!dpp_comms || hw_info.hardware_bus < 0 || !storageEnabled()) {
        return;
    }

    const int startPort = (expansion >= 0) ? expansion : 0;
    const int endPort = (expansion >= 0) ? (expansion + 1) : 2;

    for (int port = startPort; port < endPort; ++port) {
        u32 fnCode = getFunctionCodesMask(port);

        if ((fnCode & MFID_1_Storage) == 0) {
            // Not a storage device
            continue;
        }

        const std::string& gameId = settings.content.gameId;
        if (gameId.empty()) {
            return;
        }

        MapleMsg msg{};
        msg.command = 33;
        msg.destAP = (hw_info.hardware_bus << 6) | (1u << port);
        msg.originAP = hw_info.hardware_bus << 6;
        msg.pushData(MFID_1_Storage);
        msg.pushData(gameId.data(), std::min<u32>(gameId.size() + 1, 12));
        msg.size = 4;

        dpp_comms->send(msg, timeout_ms);
    }
}

void DreamPicoPort::onGameStarted()  {
    GamepadDreamLink::onGameStarted();
    sendGameId();
}

void DreamPicoPort::onGameTermination()  {
    GamepadDreamLink::onGameTermination();
    // Need a short delay to wait for last screen draw to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Reset screen to selected port
    sendPort();
}

int DreamPicoPort::fcPortToDppPort(int forPort) {
    // Flycast uses port index 5 for main peripheral and 0 is the first sub-peripheral slot
    // DreamPicoPort uses port index 0 for main peripheral and 1 is the first sub-peripheral slot
    if (forPort >= 5) {
        return 0;
    } else {
        return forPort + 1;
    }
}

int DreamPicoPort::dppPortToFcPort(int forPort) {
    if (forPort == 0) {
        return 5;
    } else {
        return forPort - 1;
    }
}

const DreamPicoPort::HardwareInfo& DreamPicoPort::getHardwareInfo() {
    return hw_info;
}

u32 DreamPicoPort::getFunctionCodesMask(int forPort) const  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    forPort = fcPortToDppPort(forPort);
    u32 mask = 0;
    if ((int)peripherals.size() > forPort) {
        for (const auto& peripheral : peripherals[forPort]) {
            mask |= peripheral[0];
        }
    }
    // swap bytes to get the correct function code
    return SWAP32(mask);
}

std::array<u32, 3> DreamPicoPort::getFunctionDefinitions(int forPort) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    forPort = fcPortToDppPort(forPort);
    std::array<u32, 3> arr{0, 0, 0};
    if ((int)peripherals.size() > forPort) {
        std::size_t idx = 0;
        for (const auto& peripheral : peripherals[forPort]) {
            arr[idx++] = SWAP32(peripheral[1]);
            if (idx >= 3) break;
        }
    }
    return arr;
}

int DreamPicoPort::getDefaultBus() const {
    return hw_info.hardware_bus;
}

void DreamPicoPort::changeBus(int newBus)  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (software_bus == newBus)
        return;

    // Show change notice only after first connection attempt
    if (connect_attempted) {
        NOTICE_LOG(
            INPUT,
            "DreamPicoPort[%s] -> DreamPicoPort[%s]",
            getLocDesc().c_str(),
            getLocDesc(newBus).c_str()
        );
    }

    software_bus = newBus;
    registerLink(software_bus, ALL_PORTS_MASK); // will automatically unregister from previous bus
    setMapleDevices();
    if (dpp_comms) {
        dpp_comms->changeSoftwareBus(software_bus);
        sendPort();
    }
}

void DreamPicoPort::registered()  {
    registerLink(software_bus, ALL_PORTS_MASK);
}

const char* DreamPicoPort::getName() const  {
    return device_name.c_str();
}

const char* DreamPicoPort::getProductName() const  {
    return hw_info.getProductName();
}

void DreamPicoPort::setMapleDevices()
{
    if (!DreamLink::isValidBus(software_bus))
        return;

    u32 portOneFn = getFunctionCodesMask(0);
    if (portOneFn & MFID_1_Storage) {
        if (storageEnabled() && EventManager::isGameRunning())
        {
            sendGameId(0);
        }
    }

    u32 portTwoFn = getFunctionCodesMask(1);
    if (portTwoFn & MFID_1_Storage) {
        if (storageEnabled() && EventManager::isGameRunning())
        {
            sendGameId(1);
        }
    }
}

bool DreamPicoPort::isConnected()  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return (dpp_comms && dpp_comms->isConnected());
}

const char* DreamPicoPort::getIssueDescription() const  {
    if (update_required) {
        return i18n::T("Firmware Update Required");
    } else {
        return GamepadDreamLink::getIssueDescription();
    }
}

std::shared_ptr<maple_device> DreamPicoPort::createMapleDevice(int bus, int port)  {
    if (port == 5) {
        return std::make_shared<MapleLinkMainDevice>(MapleLink(shared_from_this(), bus, port));
    } else {
        return std::make_shared<DppMapleLinkDevice>(MapleLink(shared_from_this(), bus, port));
    }
}

void DreamPicoPort::connect() {
    connect(true);
}

void DreamPicoPort::connect(bool autoReconnect) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    connect_attempted = true;
    connect_requested = true;

    internalConnect();

    if (!isConnected() && autoReconnect) {
        // Retry again later
        scheduleConnectRetry();
    }
}

void DreamPicoPort::disconnect()  {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    internalDisconnect();

    connect_requested = false;
}

void DreamPicoPort::sendPort() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (dpp_comms) {
        dpp_comms->sendPort(timeout_ms);
    }
}

int DreamPicoPort::hardwareBus() const {
    return hw_info.hardware_bus;
}

bool DreamPicoPort::isSingleDevice() const {
    return hw_info.is_single_device;
}

std::string DreamPicoPort::getLocDesc(int software_bus) const {
    if (software_bus < 0) {
        software_bus = this->software_bus;
    }
    return getBusDescription(software_bus, hw_info.hardware_bus);
}

bool DreamPicoPort::queryPeripherals(bool clearOnFailure) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    std::vector<std::vector<std::array<uint32_t, 2>>> prev = peripherals;

    if (clearOnFailure) {
        peripherals.clear();
    }

    if (!isConnected()) {
        return false;
    }

    std::optional<std::vector<std::vector<std::array<uint32_t, 2>>>> optPeriph = dpp_comms->getPeripherals(timeout_ms);

    if (!optPeriph) {
        return false;
    }

    peripherals = std::move(optPeriph.value());

    // If game is running, send game ID to any newly attached VMUs
    if (EventManager::isGameRunning()) {
        auto portContainsMemory = [](const std::vector<std::array<uint32_t, 2>>& portData) {
            bool containsMemory = false;
            for (const auto& fns : portData) {
                if (SWAP32(fns[0]) == MFID_1_Storage) {
                    containsMemory = true;
                    break;
                }
            }
            return containsMemory;
        };

        for (int port = 0; port < peripherals.size(); ++port) {
            if (
                portContainsMemory(peripherals[port]) &&
                (port >= prev.size() || !portContainsMemory(prev[port]))
            ) {
                sendGameId(dppPortToFcPort(port));
            }
        }
    }

    const std::string portCharStr = std::string(1, getPortCharForBus(software_bus));
    const u32 mainCode = getFunctionCodesMask(5);

    if (mainCode != 0) {
        std::string deviceSummary(fnToName(getFunctionCodesMask(5)));

        for (int i = MAPLE_FIRST_EXT_DEV_IDX; i <= MAPLE_LAST_EXT_DEV_IDX; ++i) {
            const u32 code = getFunctionCodesMask(i);
            if (code != 0) {
                const std::string extDesc = portCharStr + std::string(1, '1' + i - MAPLE_FIRST_EXT_DEV_IDX);
                deviceSummary += ", " + extDesc + ": " + fnToName(code);
            }
        }

        NOTICE_LOG(INPUT, "DreamPicoPort[%s]: %s", getLocDesc().c_str(), deviceSummary.c_str());
    } else {
        NOTICE_LOG(INPUT, "DreamPicoPort[%s]: No peripherals connected", getLocDesc().c_str());
    }

    return true;
}

std::string DreamPicoPort::getSerialFromName(const std::string& name)
{
    std::string serial;

    size_t dash_pos = name.find('-');

    if (dash_pos != std::string::npos) {
        size_t start_pos = dash_pos + 1;
        size_t end_pos = name.find(' ', start_pos);
        if (end_pos == std::string::npos) {
            end_pos = name.length();
        }

        // Serials are normally 16 characters, but check for at least 10 to account for any future changes
        if ((start_pos + 10) <= end_pos) {
            serial = name.substr(start_pos, end_pos - start_pos);
        }
    }

    return serial;
}

void DreamPicoPort::internalConnect() {
    // Timeout is 1 second while establishing connection
    timeout_ms = std::chrono::seconds(1);

    if (isConnected()) {
        sendPort();
        return;
    }

    // Attempt to connect to new API
    if (!hw_info.serial_number.empty()) {
        dpp_comms = std::make_unique<ApiDreamPicoPortComms>(hw_info, software_bus);

        if (dpp_comms->isConnected() && dpp_comms->initialize(timeout_ms)) {
            // Connected and initialized!
            NOTICE_LOG(INPUT, "DreamPicoPort[%s] API connected", getLocDesc().c_str());

            std::array<dpp_api::GamepadConnectionState, 4> gamepads = dpp_comms->getConnectedGamepads();
            if (
                hw_info.hardware_bus >= gamepads.size() ||
                gamepads[hw_info.hardware_bus] == dpp_api::GamepadConnectionState::UNAVAILABLE
            ) {
                WARN_LOG(
                    INPUT,
                    "DreamPicoPort[%s]: Hardware bus lookup failed",
                    getLocDesc().c_str()
                );
            }
        } else {
            update_required = dpp_comms->isUpdateRequired();
            dpp_comms.reset();
        }
    } else {
        NOTICE_LOG(INPUT, "Serial number for DreamPicoPort[%s] not found", getLocDesc().c_str());
    }

    if (isConnected()) {
        sendPort();
    } else {
        internalDisconnect();
        return;
    }

    if (!queryPeripherals()) {
        internalDisconnect();
        return;
    }

    // Timeout is extended to 5 seconds for all other communication after connection
    timeout_ms = std::chrono::seconds(5);

    // Query peripheral after 3.5 seconds on first connection attempt
    // This is because gamepad_btn_input event handler isn't active for a few seconds after physical connection. An
    // older VMU without battery takes 3 seconds to boot, so this should ensure initial attachment is captured.
    std::weak_ptr<BaseDreamLink> weakThis = weak_from_this();
    gui_runOnUiThread(
        std::chrono::milliseconds(3500),
        [weakThis]()
        {
            std::shared_ptr<DreamPicoPort> link = std::dynamic_pointer_cast<DreamPicoPort>(weakThis.lock());
            if (link) {
                link->queryPeripherals();
            }
        }
    );

    setMapleDevices();
}

void DreamPicoPort::internalDisconnect() {
    bool wasConnected = (dpp_comms != nullptr);
    dpp_comms.reset();

    if (wasConnected) {
        NOTICE_LOG(INPUT, "DreamPicoPort[%s] API disconnected", getLocDesc().c_str());
    }
}

void DreamPicoPort::scheduleConnectRetry() {
    if (connect_retry_scheduled) {
        // Already scheduled
        return;
    }

    connect_retry_scheduled = true;

    std::weak_ptr<BaseDreamLink> weakThis = weak_from_this();
    gui_runOnUiThread(
        CONNECT_RETRY_DELAY,
        [weakThis]()
        {
            std::shared_ptr<DreamPicoPort> link = std::dynamic_pointer_cast<DreamPicoPort>(weakThis.lock());
            if (link) {
                link->connectionCallback();
            }
        }
    );
}

void DreamPicoPort::connectionCallback() {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    connect_retry_scheduled = false;

    if (connect_requested && !isConnected()) {
        internalConnect();

        if (!isConnected()) {
            // Retry again later
            scheduleConnectRetry();
        }
    }
}

const char* DreamPicoPort::fnToName(u32 fnCode) {
    if (fnCode == 0) {
        return "None";
    }
    else if (fnCode & MFID_0_Input) {
        return "Controller";
    } else if (fnCode & MFID_1_Storage) {
        if (fnCode & MFID_2_LCD) {
            return "VMU";
        } else if (fnCode & MFID_8_Vibration) {
            return "Jump Pack & Memory";
        } else {
            return "Memory Unit";
        }
    } else if (fnCode & MFID_4_Mic) {
        return "Microphone";
    } else if (fnCode & MFID_5_ARGun) {
        return "AR Gun";
    } else if (fnCode & MFID_6_Keyboard) {
        return "Keyboard";
    } else if (fnCode & MFID_7_LightGun) {
        return "Light Gun";
    } else if (fnCode & MFID_8_Vibration) {
        return "Jump Pack";
    } else if (fnCode & MFID_9_Mouse) {
        return "Mouse";
    } else if (fnCode & MFID_10_StorageExt) {
        return "External Storage";
    } else if (fnCode & MFID_11_Camera) {
        return "Camera";
    }

    return "Unknown";
}
