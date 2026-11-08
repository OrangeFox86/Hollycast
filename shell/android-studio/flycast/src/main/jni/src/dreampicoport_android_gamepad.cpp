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

#include "dreampicoport_android_gamepad.h"
#include <input/dreamlink/dreampicoport.h>
#include <jni.h>

#include <optional>
#include <unordered_map>
#include <mutex>
#include <cmath>

//! A DreamPicoPort device which also holds a connection object associated with the connection to a DreamPicoPort
class AndroidDreamPicoPort : public DreamPicoPort
{
public:
	//! Contains connection data to a UsbDevice associated with a DreamPicoPort
	struct UsbDeviceConnection
	{
		jni::Object connection_obj = {};
		intptr_t sys_dev = -1;
	};

	//! Contains hardware information needed to connect to a DreamPicoPort
	struct ExtendedHardwareInfo
	{
		HardwareInfo base_info;
		std::shared_ptr<UsbDeviceConnection> usb_device_connection = {};
	};

public:
	//! Constructor
	//! @param[in] bus Initial software bus
	//! @param[in] hw_info Hardware information needed to connect to a DreamPicoPort
	AndroidDreamPicoPort(int bus, ExtendedHardwareInfo& hw_info) :
		DreamPicoPort(bus, hw_info.base_info),
		usb_device_connection(hw_info.usb_device_connection)
	{}

	//! Destructor
	~AndroidDreamPicoPort()
	{
		// Ensure device connection is closed on destruction
		usb_device_connection.reset();
	}

	//! Attempt to open a connection to a UsbDevice
	//! @warning The returnedconnection should not be retained once the USB device is disconnected
	//! @param[in] env Local Java environment
	//! @param[in] usbManager A UsbManager object created from the current app context
	//! @param[in] usbDev A UsbDevice object to open
	//! @param[in] serial Serial number associated with usbDev
	//! @return nullptr if the connection could not be opened
	//! @return shared_ptr to UsbDeviceConnection which contains a file descriptor to the given usbDev
	static std::shared_ptr<UsbDeviceConnection> openUsbDeviceAndGetFd(
		JNIEnv *env,
		jobject usbManager,
		jobject usbDev,
		const std::string& serial
	) {
		// A lookup is used because up to 4 controllers may have the same serial and don't need redundant connections
		static std::unordered_map<std::string, std::weak_ptr<UsbDeviceConnection>> lookup;
		static std::mutex lookupMutex;

		std::lock_guard<std::mutex> lock(lookupMutex);

		auto iter = lookup.find(serial);
		if (iter != lookup.end()) {
			std::shared_ptr<UsbDeviceConnection> ptr = iter->second.lock();
			if (ptr) {
				// Reuse existing connection from lookup
				NOTICE_LOG(INPUT, "Reusing existing UsbDeviceConnection (fd %d) for %s", (int)ptr->sys_dev, serial.c_str());
				return ptr;
			}
			// Weak pointer no longer valid; remove it
			lookup.erase(iter);
		}

		jni::Class usbManagerClass(env->GetObjectClass(usbManager));
		jmethodID openDeviceMethod = env->GetMethodID(
			usbManagerClass,
			"openDevice",
			"(Landroid/hardware/usb/UsbDevice;)Landroid/hardware/usb/UsbDeviceConnection;"
		);

		jni::Object connection(env->CallObjectMethod(usbManager, openDeviceMethod, usbDev));

		if (connection == nullptr) {
			ERROR_LOG(INPUT, "UsbManager.OpenDevice failed");
			return nullptr;
		}

		jni::Class connectionClass(env->GetObjectClass(connection));
		jmethodID getFileDescriptorMethod = env->GetMethodID(connectionClass, "getFileDescriptor", "()I");

		jint fd = env->CallIntMethod(connection, getFileDescriptorMethod);

		if (fd < 0)
		{
			ERROR_LOG(INPUT, "Invalid file descritor received from UsbDeviceConnection");
			return nullptr;
		}

		std::shared_ptr<UsbDeviceConnection> newConnection = std::make_shared<UsbDeviceConnection>();
		// Promote to global ref so it survives past this call — required since
		// libusb will use the fd beyond the lifetime of this native call frame
		newConnection->connection_obj = connection.globalRef<jni::Object>();
		newConnection->sys_dev = (intptr_t)fd;

		NOTICE_LOG(INPUT, "Opened new UsbDeviceConnection (fd %d) for %s", (int)fd, serial.c_str());

		// Save to lookup for future use
		lookup[serial] = newConnection;

		return newConnection;
	}

private:
	//! Holds the connection object for the lifespan of the DreamPicoPort
	std::shared_ptr<UsbDeviceConnection> usb_device_connection;
};

//! Get number of devices that contain the given serial at the specified ID direction
//! @param[in] env The Java Native Interface environment object
//! @param[in] id The ID of the target device
//! @param[in] serial The serial of the target device
//! @param[in] direction -1 to search backwards or 1 to search forwards
//! @param[in] inputDeviceClass The InputDevice class pointer
//! @param[in] getDeviceMethodId Method ID of InputDevice.getDevice()
//! @param[in] getNameMethodId Method ID of InputDevice.getName()
//! @param[in] maxCount The maximum count to return (default: 3)
//! @return the count
static int get_serial_count(
	JNIEnv *env,
	int id,
	const std::string& serial,
	int direction,
	const jni::Class& inputDeviceClass,
	jmethodID getDeviceMethodId,
	jmethodID getNameMethodId,
	int maxCount = 3
)
{
	const int increment = (direction < 0) ? -1 : 1;
	int count = 0;
	static const int kMaxMisses = 10;
	jint lastKnownId = id;
	jint testId = id + increment;
	while (testId >= 0 && ((testId - lastKnownId) * increment) < kMaxMisses && count < maxCount)
	{
		jni::Object device(env->CallStaticObjectMethod(inputDeviceClass, getDeviceMethodId, testId));
		if (!device.isNull())
		{
			jni::String jName(env->CallObjectMethod(device, getNameMethodId));

			if (!jName.isNull())
			{
				std::string deviceName(jName.to_string());
				if (deviceName.find(serial) != std::string::npos)
				{
					lastKnownId = testId;
					++count;
				}
			}
		}

		testId += increment;
	}

	return count;
}

//! Attempts to locate a UsbDevice by VID, PID, and serial number
//! @param[in] env The Java Native Interface environment object
//! @param[in] usbManager A UsbManager object created from the current app context
//! @param[in] targetVid Target vendor ID
//! @param[in] targetPid Target product ID
//! @param[in] targetSerial Target serial number
//! @return nullptr if the device could not be located or permission hasn't been granted yet
//! @return a UsbDevice associated with VID, PID, and serial number otherwise
static jni::Object find_usb_device_by_vid_pid_serial(
	JNIEnv *env,
	jobject usbManager,
	jint targetVid,
	jint targetPid,
	const std::string& targetSerial
)
{
	jni::Class usbManagerClass(env->GetObjectClass(usbManager));
	jmethodID getDeviceListMethod = env->GetMethodID(usbManagerClass, "getDeviceList", "()Ljava/util/HashMap;");
	jni::Object deviceMap(env->CallObjectMethod(usbManager, getDeviceListMethod));

	if (deviceMap.isNull()) {
		NOTICE_LOG(INPUT, "UsbManager.getDeviceList() returned null while looking for %s", targetSerial.c_str());
		return nullptr;
	}

	jmethodID hasPermissionMethod = env->GetMethodID(
		usbManagerClass,
		"hasPermission",
		"(Landroid/hardware/usb/UsbDevice;)Z"
	);
	int totalDevices = 0;
	int candidates = 0;

	jni::Class mapClass(env->GetObjectClass(deviceMap));
	jmethodID valuesMethod = env->GetMethodID(mapClass, "values", "()Ljava/util/Collection;");
	jni::Object valuesCollection(env->CallObjectMethod(deviceMap, valuesMethod));

	jni::Class collectionClass(env->GetObjectClass(valuesCollection));
	jmethodID iteratorMethod = env->GetMethodID(collectionClass, "iterator", "()Ljava/util/Iterator;");
	jni::Object iterator(env->CallObjectMethod(valuesCollection, iteratorMethod));

	jni::Class iteratorClass(env->GetObjectClass(iterator));
	jmethodID hasNextMethod = env->GetMethodID(iteratorClass, "hasNext", "()Z");
	jmethodID nextMethod = env->GetMethodID(iteratorClass, "next", "()Ljava/lang/Object;");

	jni::Class usbDeviceClass(env->FindClass("android/hardware/usb/UsbDevice"));
	jmethodID getVendorIdMethod = env->GetMethodID(usbDeviceClass, "getVendorId", "()I");
	jmethodID getProductIdMethod = env->GetMethodID(usbDeviceClass, "getProductId", "()I");
	jmethodID getSerialNumberMethod = env->GetMethodID(usbDeviceClass, "getSerialNumber", "()Ljava/lang/String;");

	while (env->CallBooleanMethod(iterator, hasNextMethod)) {
		jni::Object device(env->CallObjectMethod(iterator, nextMethod));
		++totalDevices;

		jint vid = env->CallIntMethod(device, getVendorIdMethod);
		jint pid = env->CallIntMethod(device, getProductIdMethod);

		bool matches = (vid == targetVid && pid == targetPid);

		if (matches) {
			++candidates;
			bool hasPermission = false;
			if (hasPermissionMethod) {
				hasPermission = env->CallBooleanMethod(usbManager, hasPermissionMethod, (jobject)device);
				if (env->ExceptionCheck()) {
					env->ExceptionClear();
				}
			}

			jni::String deviceSerial(env->CallObjectMethod(device, getSerialNumberMethod));

			jni::Throwable throwable(env->ExceptionOccurred());
			if (!throwable.isNull()) {
				env->ExceptionClear();
				ERROR_LOG(
					INPUT,
					"getSerialNumber() exception for DreamPicoPort candidate (hasPermission=%d): '%s'",
					(int)hasPermission,
					throwable.getMessage().c_str()
				);
				matches = false;
			} else if (deviceSerial.isNull()) {
				// Permission not yet granted, or device has no serial — can't match
				NOTICE_LOG(
					INPUT,
					"DreamPicoPort candidate has null serial (hasPermission=%d) while looking for %s",
					(int)hasPermission,
					targetSerial.c_str()
				);
				matches = false;
			} else {
				const std::string serial = deviceSerial.to_string();
				matches = (serial == targetSerial);
				if (!matches) {
					NOTICE_LOG(
						INPUT,
						"DreamPicoPort candidate serial %s (hasPermission=%d) doesn't match target %s",
						serial.c_str(),
						(int)hasPermission,
						targetSerial.c_str()
					);
				}
			}
		}

		if (matches) {
			return device;
		}
	}

	NOTICE_LOG(
		INPUT,
		"UsbDevice for %s not found: %d USB device(s) listed, %d with DreamPicoPort VID/PID",
		targetSerial.c_str(),
		totalDevices,
		candidates
	);

	return jni::Object(); // null
}

//! Converts a 0-based positional index to interface index of a UsbDevice
//! @param[in] env The Java Native Interface environment object
//! @param[in] usbDevice The UsbDevice to interrogate
//! @param[in] n 0-based positional index
//! @return -1 if n is out of range
//! @return the interface ID at position n (0-based) in the sorted list of distinct interface IDs present on the device
static int get_nth_interface_id(JNIEnv *env, jobject usbDevice, int n) {
	jni::Class usbDeviceClass(env->GetObjectClass(usbDevice));
	jmethodID getInterfaceCountMethod = env->GetMethodID(usbDeviceClass, "getInterfaceCount", "()I");
	jmethodID getInterfaceMethod = env->GetMethodID(
		usbDeviceClass,
		"getInterface",
		"(I)Landroid/hardware/usb/UsbInterface;"
	);

	jint interfaceCount = env->CallIntMethod(usbDevice, getInterfaceCountMethod);

	jni::Class usbInterfaceClass(env->FindClass("android/hardware/usb/UsbInterface"));
	jmethodID getIdMethod = env->GetMethodID(usbInterfaceClass, "getId", "()I");

	std::vector<jint> interfaceIds;
	interfaceIds.reserve(interfaceCount);

	for (jint i = 0; i < interfaceCount; i++) {
		jni::Object usbInterface(env->CallObjectMethod(usbDevice, getInterfaceMethod, i));
		jint id = env->CallIntMethod(usbInterface, getIdMethod);
		interfaceIds.push_back(id);
	}

	std::sort(interfaceIds.begin(), interfaceIds.end());
	interfaceIds.erase(std::unique(interfaceIds.begin(), interfaceIds.end()), interfaceIds.end());

	if (n < 0 || n >= static_cast<jint>(interfaceIds.size())) {
		return -1; // out of range
	}

	return interfaceIds[n];
}

static const int kPreferredHardwareBusNone = -1;
static const int kPreferredHardwareBusErr = -2;

//! DreamPicoPort's RX range identifies the hardware bus in version 1.2.5 and later.
//! @param[in] env The Java Native Interface environment object
//! @param[in] inputDevice The InputDevice object
//! @param[in] inputDeviceClass The InputDevice class
//! @return 0-3 when the descriptor-conveyed RX range can be resolved
//! @return kPreferredHardwareBusNone if device doesn't contain the RX range
//! @return kPreferredHardwareBusErr on error
static int get_preferred_hardware_bus(JNIEnv *env, jobject inputDevice, const jni::Class& inputDeviceClass) {
	if (inputDevice == nullptr || inputDeviceClass.isNull()) {
		ERROR_LOG(INPUT, "InputDevice object or class is null");
		std::abort();
	}

	jmethodID getMotionRangeMethodId = env->GetMethodID(
		inputDeviceClass,
		"getMotionRange",
		"(I)Landroid/view/InputDevice$MotionRange;"
	);
	if (!getMotionRangeMethodId) {
		ERROR_LOG(INPUT, "Failed to locate InputDevice.getMotionRange()");
		std::abort();
	}

	jni::Object motionRange(env->CallObjectMethod(
		inputDevice,
		getMotionRangeMethodId,
		AMOTION_EVENT_AXIS_RX
	));

	if (env->ExceptionCheck()) {
		ERROR_LOG(INPUT, "Exception occurred when calling InputDevice.getMotionRange()");
		env->ExceptionClear();
		return kPreferredHardwareBusErr;
	}

	if (motionRange.isNull()) {
		return kPreferredHardwareBusNone;
	}

	jni::Class motionRangeClass(env->GetObjectClass(motionRange));
	jmethodID getResolutionMethodId = env->GetMethodID(motionRangeClass, "getResolution", "()F");
	if (!getResolutionMethodId) {
		ERROR_LOG(INPUT, "Failed to locate InputDevice.MotionRange.getResolution()");
		std::abort();
	}

	const jfloat dialResolution = env->CallFloatMethod(motionRange, getResolutionMethodId);
	if (env->ExceptionCheck()) {
		ERROR_LOG(INPUT, "Exception occurred when calling InputDevice.MotionRange.getResolution()");
		env->ExceptionClear();
		return kPreferredHardwareBusErr;
	}

	if (dialResolution == 0.0f) {
		return kPreferredHardwareBusNone;
	}

	// The dialResolution should about equal 2.0 / (player index + 1)
	const int playerIdx = std::lround(2.0 / dialResolution) - 1;
	if (playerIdx >= 0 && playerIdx < 4) {
		return playerIdx;
	}

	ERROR_LOG(INPUT, "DreamPicoPort dial has unexpected resolution %f", dialResolution);
	return kPreferredHardwareBusErr;
}

//! Only to be called during instantiation to determine hardware information
//! @param[in] env The Java Native Interface environment object
//! @param[in] usbManager A UsbManager object created from the current app context
//! @param[in] id ID of an InputDevice associated with a DreamPicoPort
//! @param[in] name Name of the InputDevice (will contain the serial number of the DreamPicoPort)
//! @return std::nullopt if hardware information couldn't be retrieved due to failure or permissions issue
//! @return the hardware information of the given DreamPicoPort otherwise
static std::optional<AndroidDreamPicoPort::ExtendedHardwareInfo> parse_hw_info(
	JNIEnv *env,
	jobject usbManager,
	int id,
	const std::string& name
)
{
	AndroidDreamPicoPort::ExtendedHardwareInfo hwInfo;
	hwInfo.base_info.serial_number = DreamPicoPort::getSerialFromName(name);

	if (hwInfo.base_info.serial_number.empty())
	{
		ERROR_LOG(INPUT, "Failed to retrieve serial from DreamPicoPort name: %s", name.c_str());
		return std::nullopt;
	}

	// Attempt to retrieve the UsbDevice for this serial number
	jni::Object usbDev = find_usb_device_by_vid_pid_serial(
		env,
		usbManager,
		DreamPicoPort::VID,
		DreamPicoPort::PID,
		hwInfo.base_info.serial_number
	);

	if (usbDev.isNull())
	{
		// Probably don't have permission yet
		NOTICE_LOG(
			INPUT,
			"Failed to retrieve DreamPicoPort UsbDevice for %s (InputDevice id %d, name '%s')",
			hwInfo.base_info.serial_number.c_str(),
			id,
			name.c_str()
		);
		return std::nullopt;
	}

	jni::Class inputDeviceClass(env->FindClass("android/view/InputDevice"));
	if (inputDeviceClass.isNull())
	{
		ERROR_LOG(INPUT, "Failed to locate android/view/InputDevice");
		return std::nullopt;
	}

	jmethodID getDeviceMethodId = env->GetStaticMethodID(inputDeviceClass, "getDevice", "(I)Landroid/view/InputDevice;");
	if (!getDeviceMethodId)
	{
		ERROR_LOG(INPUT, "Failed to locate InputDevice.getDevice()");
		return std::nullopt;
	}

	jmethodID getNameMethodId = env->GetMethodID(inputDeviceClass, "getName", "()Ljava/lang/String;");
	if (!getNameMethodId)
	{
		ERROR_LOG(INPUT, "Failed to locate InputDevice.getName()");
		return std::nullopt;
	}

	jni::Object inputDevice(env->CallStaticObjectMethod(inputDeviceClass, getDeviceMethodId, id));
	const int preferredHardwareBus = get_preferred_hardware_bus(env, inputDevice, inputDeviceClass);

	if (preferredHardwareBus >= 0)
	{
		// Input descriptor has identified A/B/C/D from the RX range.
		hwInfo.base_info.hardware_bus = preferredHardwareBus;

		// Determine if there are any other devices with this serial
		if (
			get_serial_count(
				env,
				id,
				hwInfo.base_info.serial_number,
				1,
				inputDeviceClass,
				getDeviceMethodId,
				getNameMethodId,
				1
			) > 0 || get_serial_count(
				env,
				id,
				hwInfo.base_info.serial_number,
				-1,
				inputDeviceClass,
				getDeviceMethodId,
				getNameMethodId,
				1
			) > 0
		) {
			hwInfo.base_info.is_single_device = false;
		}
	}
	else if (preferredHardwareBus == kPreferredHardwareBusNone)
	{
		hwInfo.base_info.hardware_bus = get_serial_count(
			env,
			id,
			hwInfo.base_info.serial_number,
			-1,
			inputDeviceClass,
			getDeviceMethodId,
			getNameMethodId
		);

		if (hwInfo.base_info.hardware_bus == 0)
		{
			if (
				get_serial_count(
					env,
					id,
					hwInfo.base_info.serial_number,
					1,
					inputDeviceClass,
					getDeviceMethodId,
					getNameMethodId,
					1
				) > 0
			)
			{
				hwInfo.base_info.is_single_device = false;
			}
		}
		else
		{
			hwInfo.base_info.is_single_device = false;
		}

		// Interfaces 0-3 correspond to the four gamepad ports. If only ports B and D are connected,
		// their interface IDs would otherwise be seen as 0 and 1; remapping them to their true
		// slot indices (1 and 3) keeps hardware_bus consistent with the physical port layout.
		hwInfo.base_info.hardware_bus = get_nth_interface_id(env, usbDev, hwInfo.base_info.hardware_bus);

		WARN_LOG(
			INPUT,
			"DreamPicoPort guessed hardware bus %i for controller - "
			"please update firmware to 1.2.5 or later for more robust Android support",
			hwInfo.base_info.hardware_bus
		);
	}
	else
	{
		return std::nullopt;
	}

	hwInfo.usb_device_connection =
		AndroidDreamPicoPort::openUsbDeviceAndGetFd(env, usbManager, usbDev, hwInfo.base_info.serial_number);

	if (!hwInfo.usb_device_connection)
	{
		ERROR_LOG(INPUT, "Failed to open file descriptor to DreamPicoPort device");
		return std::nullopt;
	}

	hwInfo.base_info.sys_dev = hwInfo.usb_device_connection->sys_dev;

	return hwInfo;
}

//! Attempts to make a new AndroidDreamPicoPort device
//! @param[in] env The Java Native Interface environment object
//! @param[in] usbManager A UsbManager object created from the current app context
//! @param[in] maple_port The requested maple port index to use
//! @param[in] id ID of an InputDevice associated with a DreamPicoPort
//! @param[in] name Name of the InputDevice (will contain the serial number of the DreamPicoPort)
//! @return nullptr if connection could not be made (usually due to waiting for permission from user)
//! @return a new AndroidDreamPicoPort otherwise
static std::shared_ptr<AndroidDreamPicoPort> make_dpp(
	JNIEnv *env,
	jobject usbManager,
	int maple_port,
	int id,
	const std::string& name
)
{
	std::optional<AndroidDreamPicoPort::ExtendedHardwareInfo> hwInfo = parse_hw_info(env, usbManager, id, name);

	if (!hwInfo.has_value())
	{
		return nullptr;
	}

	if (hwInfo->base_info.hardware_bus >= 0 && hwInfo->usb_device_connection)
	{
		return std::make_shared<AndroidDreamPicoPort>(maple_port, hwInfo.value());
	}

	ERROR_LOG(INPUT, "DreamPicoPort hardware info is invalid");

	return nullptr;
}

DreamPicoPortAndroidGamepad::DreamPicoPortAndroidGamepad(
	JNIEnv *env,
	jobject usbManager,
	int maple_port,
	const AndroidGamepadDevice::AndroidJoystickData& joystickData
) :
	DreamLinkAndroidGamepad(
		make_dpp(env, usbManager, maple_port, joystickData.id, joystickData.joyname),
		maple_port,
		joystickData
	),
	dpp(std::dynamic_pointer_cast<AndroidDreamPicoPort>(dreamlink)),
	android_name(joystickData.joyname)
{
	// The name will be the main device name, not the specific gamepad name
	// e.x. "OrangeFox86 DreamPicoPort-E66141040371972A v1.2.4"

	if (dpp) {
		_name = dpp->getName();

		const DreamPicoPort::HardwareInfo& hw_info = dpp->getHardwareInfo();

		if (!hw_info.serial_number.empty()) {
			// Ensure this is ordered by product name, serial, and port char
			_sort_id = (
				hw_info.getProductName() + std::string("_") +
				hw_info.serial_number + std::string("_") +
				std::string(1, hw_info.getPortChar())
			);
			// Locking to name, which includes A-D, plus serial number will ensure correct enumeration every time
			_unique_id = hw_info.getName("", true) + std::string("_") + hw_info.serial_number;
			// Reload mapping now that unique ID changed
			loadMapping();
		}

		int bus = dpp->getDefaultBus();
		if (DreamLink::isValidBus(bus))
			set_maple_port(bus);
	} else {
		_name = "DreamPicoPort";
	}
}

// Need to define destructor in source because DreamPicoPort has a forward declaration
DreamPicoPortAndroidGamepad::~DreamPicoPortAndroidGamepad()
{
}

const char *DreamPicoPortAndroidGamepad::get_button_name(u32 code)
{
	using namespace i18n;
	switch (static_cast<s32>(code)) {
		case static_cast<s32>(ButtonCode::D): return "D";
		case static_cast<s32>(ButtonCode::UP_B): return T("DPad2 Up");
		case static_cast<s32>(ButtonCode::DOWN_B): return T("DPad2 Down");
		case static_cast<s32>(ButtonCode::LEFT_B): return T("DPad2 Left");
		case static_cast<s32>(ButtonCode::RIGHT_B): return T("DPad2 Right");

		// Alternate directional buttons
		case static_cast<s32>(ButtonCode::ALT_UP): return T("Alt Up");
		case static_cast<s32>(ButtonCode::ALT_DOWN): return T("Alt Down");
		case static_cast<s32>(ButtonCode::ALT_LEFT): return T("Alt Left");
		case static_cast<s32>(ButtonCode::ALT_RIGHT): return T("Alt Right");

		// These buttons are normally not physically accessible but are mapped on DreamPicoPort
		case static_cast<s32>(ButtonCode::VMU1_BUTTON_A): return T("VMU1 A");
		case static_cast<s32>(ButtonCode::VMU1_BUTTON_B): return T("VMU1 B");
		case static_cast<s32>(ButtonCode::VMU1_BUTTON_UP): return T("VMU1 Up");
		case static_cast<s32>(ButtonCode::VMU1_BUTTON_DOWN): return T("VMU1 Down");
		case static_cast<s32>(ButtonCode::VMU1_BUTTON_LEFT): return T("VMU1 Left");
		case static_cast<s32>(ButtonCode::VMU1_BUTTON_RIGHT): return T("VMU1 Right");

		case static_cast<s32>(ButtonCode::CHANGE_EVENT): return T("Device Change");

		default: return DreamLinkAndroidGamepad::get_button_name(code); // use the default name
	}
}

const char *DreamPicoPortAndroidGamepad::get_axis_name(u32 code)
{
	using namespace i18n;
	switch (static_cast<s32>(code)) {
		case static_cast<s32>(AxisCode::LEFT_TRIGGER): return T("LT");
		case static_cast<s32>(AxisCode::RIGHT_TRIGGER): return T("RT");

		default: return DreamLinkAndroidGamepad::get_axis_name(code); // use the default name
	}
}

void DreamPicoPortAndroidGamepad::close(JNIEnv *env)
{
	DreamLinkAndroidGamepad::close(env);
	dpp.reset();
}

bool DreamPicoPortAndroidGamepad::identify(int vendorId, int productId)
{
	return (vendorId == DreamPicoPort::VID && productId == DreamPicoPort::PID);
}

bool DreamPicoPortAndroidGamepad::gamepad_btn_input(u32 code, bool pressed)
{
	if (static_cast<s32>(code) == static_cast<s32>(ButtonCode::CHANGE_EVENT) && !pressed)
	{
		if (dpp)
		{
			dpp->queryPeripherals(false);
		}
	}

	return DreamLinkAndroidGamepad::gamepad_btn_input(code, pressed);
}

void DreamPicoPortAndroidGamepad::setCustomMapping(const std::shared_ptr<InputMapping>& mapping)
{
	// Since this is a real DC controller, no deadzone adjustment is needed
	mapping->dead_zone = 0.0f;
	// Map the things not set by default
	mapping->set_button(DC_BTN_C, static_cast<u32>(ButtonCode::C));
	mapping->set_button(DC_BTN_Z, static_cast<u32>(ButtonCode::Z));
	mapping->set_button(DC_BTN_D, static_cast<u32>(ButtonCode::D));
	mapping->set_button(DC_DPAD2_UP, static_cast<u32>(ButtonCode::UP_B));
	mapping->set_button(DC_DPAD2_DOWN, static_cast<u32>(ButtonCode::DOWN_B));
	mapping->set_button(DC_DPAD2_LEFT, static_cast<u32>(ButtonCode::LEFT_B));
	mapping->set_button(DC_DPAD2_RIGHT, static_cast<u32>(ButtonCode::RIGHT_B));
	mapping->set_axis(DC_AXIS_LT, static_cast<u32>(AxisCode::LEFT_TRIGGER), true);
	mapping->set_axis(DC_AXIS_RT, static_cast<u32>(AxisCode::RIGHT_TRIGGER), true);
	mapping->set_axis(DC_AXIS2_LEFT, static_cast<u32>(AxisCode::RIGHT_X), false);
	mapping->set_axis(DC_AXIS2_RIGHT, static_cast<u32>(AxisCode::RIGHT_X), true);
	mapping->set_axis(DC_AXIS2_UP, static_cast<u32>(AxisCode::RIGHT_Y), false);
	mapping->set_axis(DC_AXIS2_DOWN, static_cast<u32>(AxisCode::RIGHT_Y), true);
}
