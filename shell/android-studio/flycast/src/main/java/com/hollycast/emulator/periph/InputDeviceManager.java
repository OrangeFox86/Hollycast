/*
	Portions Copyright 2026 The Hollycast Authors
 */

package com.hollycast.emulator.periph;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;

import androidx.core.content.ContextCompat;

import com.hollycast.emulator.Emulator;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class InputDeviceManager implements InputManager.InputDeviceListener {
    public static final int VIRTUAL_GAMEPAD_ID = 0x12345678;

    //! Holds information about an InputDevice which is needed to construct a new joystick
    public static final class JoystickData {
        public final int vid;
        public final int pid;
        public final String joyname;
        public final String uniqueId;
        public final int[] fullAxes;
        public final int[] halfAxes;
        public final boolean hasRumble;

        public JoystickData(int vid, int pid, String joyname, String uniqueId, int[] fullAxes, int[] halfAxes, boolean hasRumble) {
            this.vid = vid;
            this.pid = pid;
            this.joyname = joyname;
            this.uniqueId = uniqueId;
            this.fullAxes = fullAxes;
            this.halfAxes = halfAxes;
            this.hasRumble = hasRumble;
        }
    }

    //! Intent name when permission is requested to access a UsbDevice
    private static final String ACTION_USB_PERMISSION = "com.hollycast.emulator.USB_PERMISSION";

    static { System.loadLibrary("Hollycast"); }
    private static final InputDeviceManager INSTANCE = new InputDeviceManager();
    private InputManager inputManager;
    private UsbManager usbManager;
    private int maple_port = 0;

    private final Set<String> pendingPermissionRequests = new HashSet<>();
    private boolean receiverRegistered = false;

    private boolean hasTouchscreen = false;

    private static class VibrationParams {
        float power;
        float inclination;
        long stopTime;
    }
    private Map<Integer, VibrationParams> vibParams = new HashMap<>();
    private Set<Integer> knownDevices = new HashSet<>();

    //! The receiver which is activated after the user handles the UsbDevice permission dialog (approve/deny)
    private final BroadcastReceiver usbPermissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (ACTION_USB_PERMISSION.equals(intent.getAction())) {
                synchronized (pendingPermissionRequests) {
                    UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);

                    if (device != null) {
                        pendingPermissionRequests.remove(device.getDeviceName());
                    }

                    if (granted && device != null) {
                        permissionGranted(
                            usbManager,
                            getKnownDeviceIdsByVidPid(device.getVendorId(), device.getProductId())
                        );
                    }

                    // Only unregister once every in-flight request has been resolved
                    if (pendingPermissionRequests.isEmpty() && receiverRegistered) {
                        Emulator.getAppContext().unregisterReceiver(this);
                        receiverRegistered = false;
                    }
                }
            }
        }
    };

    public void handleUsbDeviceAttached(UsbDevice device) {
        if (usbManager.hasPermission(device)) {
            permissionGranted(usbManager, getKnownDeviceIdsByVidPid(device.getVendorId(), device.getProductId()));
        }
    }

    public InputDeviceManager()
    {
        init();
    }

    public void startListening(Context applicationContext)
    {
        knownDevices.clear();
        maple_port = 0;
        hasTouchscreen = applicationContext.getPackageManager().hasSystemFeature("android.hardware.touchscreen");
        if (hasTouchscreen)
        {
            joystickAdded(usbManager, VIRTUAL_GAMEPAD_ID, 0);
        }
        inputManager = (InputManager)applicationContext.getSystemService(Context.INPUT_SERVICE);
        inputManager.registerInputDeviceListener(this, null);
        usbManager = (UsbManager)applicationContext.getSystemService(Context.USB_SERVICE);

        // Parse already-known devices
        int[] ids = InputDevice.getDeviceIds();
        for (int i = 0; i < ids.length; i++) {
            int id = ids[i];
            onInputDeviceAdded(id);
        }
    }

    public void stopListening()
    {
        synchronized (pendingPermissionRequests) {
            if (receiverRegistered) {
                Emulator.getAppContext().unregisterReceiver(usbPermissionReceiver);
                receiverRegistered = false;
            }
            pendingPermissionRequests.clear();
        }

        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(this);
            inputManager = null;
        }
        joystickRemoved(VIRTUAL_GAMEPAD_ID);
    }

    //! Ensure that the usbPermissionReceiver is registered in order to listen to permission request approval
    private void ensureReceiverRegistered() {
        if (receiverRegistered) {
            return;
        }

        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);

        ContextCompat.registerReceiver(
            Emulator.getAppContext(),
            usbPermissionReceiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED
        );

        receiverRegistered = true;
    }

    @Override
    public void onInputDeviceAdded(int id) {
        if (id == 0)
            return;
        if (knownDevices.contains(id))
            return;
        InputDevice device = InputDevice.getDevice(id);

        if (device != null) {
            int vid = device.getVendorId();
            int pid = device.getProductId();
            if (isPriorityDevice(vid, pid)) {
                // Create this device now
                createDevice(id);
            }
        }
    }

    @Override
    public void onInputDeviceRemoved(int i) {
        if (maple_port > 0)
            maple_port--;
        joystickRemoved(i);
        knownDevices.remove(i);
    }

    @Override
    public void onInputDeviceChanged(int i) {
    }

    private Vibrator getVibrator(int i) {
        if (i == VIRTUAL_GAMEPAD_ID) {
            return (Vibrator)Emulator.getAppContext().getSystemService(Context.VIBRATOR_SERVICE);
        }
        else {
            InputDevice device = InputDevice.getDevice(i);
            if (device == null)
                return null;
            Vibrator vibrator = device.getVibrator();
            return vibrator.hasVibrator() ? vibrator : null;
        }
    }

    private void vibrate(Vibrator vibrator, long duration_ms, float power)
    {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            int ipow = Math.min((int)(power * 255), 255);
            if (ipow >= 1)
                vibrator.vibrate(VibrationEffect.createOneShot(duration_ms, ipow));
            else
                vibrator.cancel();
        }
        else
            vibrator.vibrate(duration_ms);
    }

    // Called from native code
    // returns false if the device has no vibrator
    private boolean rumble(int i, float power, float inclination, int duration_ms)
    {
        Vibrator vibrator = getVibrator(i);
        if (vibrator == null)
            return false;
        if (i == VIRTUAL_GAMEPAD_ID) {
            if (Emulator.vibrationPower == 0)
                return true;
            power *= Emulator.vibrationPower / 100.f;
        }

        VibrationParams params;
        synchronized (this) {
            params = vibParams.get(i);
            if (params == null) {
                params = new VibrationParams();
                vibParams.put(i, params);
            }
        }
        if (power != 0) {
            params.stopTime = System.currentTimeMillis() + duration_ms;
            if (inclination > 0)
                params.inclination = inclination * power;
            else
                params.inclination = 0;
        }
        params.power = power;
        VibratorThread.getInstance().setVibrating();

        return true;
    }

    public boolean updateRumble()
    {
        List<Integer> ids;
        synchronized (this) {
            ids = new ArrayList<Integer>(vibParams.keySet());
        }
        boolean active = false;
        for (int id : ids) {
            if (updateRumble(id))
                active = true;
        }
        return active;
    }

    private boolean updateRumble(int i)
    {
        Vibrator vibrator = getVibrator(i);
        VibrationParams params;
        synchronized (this) {
            params = vibParams.get(i);
        }
        if (vibrator == null || params == null)
            return false;
        long remTime = params.stopTime - System.currentTimeMillis();
        if (remTime <= 0 || params.power == 0) {
            params.power = 0;
            params.inclination = 0;
            vibrator.cancel();
            return false;
        }
        if (params.inclination > 0)
            vibrate(vibrator, remTime, params.inclination * remTime);
        else
            vibrate(vibrator, remTime, params.power);
        return true;
    }

    public void stopRumble()
    {
        List<Integer> ids;
        synchronized (this) {
            ids = new ArrayList<Integer>(vibParams.keySet());
        }
        for (int id : ids) {
            Vibrator vibrator = getVibrator(id);
            if (vibrator != null)
                vibrator.cancel();
        }
    }

    public boolean hasTouchscreen() {
        return hasTouchscreen;
    }

    private boolean createDevice(int id)
    {
        if (id == 0)
            return false;
        if (knownDevices.contains(id))
            return true;
        InputDevice device = InputDevice.getDevice(id);
        if (device == null || (device.getSources() & InputDevice.SOURCE_CLASS_BUTTON) != InputDevice.SOURCE_CLASS_BUTTON)
            return false;
        int port = 0;
        if ((device.getSources() & InputDevice.SOURCE_CLASS_JOYSTICK) == InputDevice.SOURCE_CLASS_JOYSTICK) {
            port = this.maple_port == 3 ? 3 : this.maple_port++;
        }

        int vid = device.getVendorId();
        int pid = device.getProductId();
        if (isPermissionRequired(vid, pid))
        {
            requestUsbPermission(vid, pid);
        }

        joystickAdded(usbManager, id, port);

        knownDevices.add(id);
        return true;
    }
    public boolean buttonEvent(int id, int button, boolean pressed)
    {
        if (!createDevice(id))
            return false;
        return joystickButtonEvent(id, button, pressed);
    }
    public boolean axisEvent(int id, int button, int value)
    {
        if (!createDevice(id))
            return false;
        return joystickAxisEvent(id, button, value);
    }

    public static InputDeviceManager getInstance() {
        return INSTANCE;
    }

    //! Retrieve joystick data describing the joystick at the given ID
    public static JoystickData getJoystickData(int id) {
        InputDevice device = InputDevice.getDevice(id);
        if (device == null) {
            return null;
        }

        List<InputDevice.MotionRange> axes = device.getMotionRanges();
        List<Integer> fullAxes = new ArrayList<>();
        List<Integer> halfAxes = new ArrayList<>();
        for (InputDevice.MotionRange range : axes) {
            if ((range.getSource() & InputDevice.SOURCE_CLASS_MASK) != InputDevice.SOURCE_CLASS_JOYSTICK)
                // Ignore mouse/touchpad axes
                continue;
            if (range.getMin() == 0)
                halfAxes.add(range.getAxis());
            else
                fullAxes.add(range.getAxis());
        }

        int[] fullAxesArray = new int[fullAxes.size()];
        for (int i = 0; i < fullAxesArray.length; i++) {
            fullAxesArray[i] = fullAxes.get(i);
        }

        int[] halfAxesArray = new int[halfAxes.size()];
        for (int i = 0; i < halfAxesArray.length; i++) {
            halfAxesArray[i] = halfAxes.get(i);
        }

        return new JoystickData(
            device.getVendorId(),
            device.getProductId(),
            device.getName(),
            device.getDescriptor(),
            fullAxesArray,
            halfAxesArray,
            INSTANCE.getVibrator(id) != null
        );
    }

    //! Request permission to access any USB devices with the given VID and PID if permission isn't already granted
    private void requestUsbPermission(int vendorId, int productId) {
        HashMap<String, UsbDevice> deviceList = usbManager.getDeviceList();

        for (final UsbDevice usbDevice : deviceList.values()) {
            if (usbDevice.getVendorId() == vendorId && usbDevice.getProductId() == productId) {
                // This is the device's unique kernel file path
                final String devName = usbDevice.getDeviceName();
                synchronized (pendingPermissionRequests) {
                    if (!usbManager.hasPermission(usbDevice) && !pendingPermissionRequests.contains(devName)) {

                        pendingPermissionRequests.add(devName);
                        ensureReceiverRegistered();

                        new Handler(Looper.getMainLooper()).post(() -> {
                            int flags = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                                ? PendingIntent.FLAG_MUTABLE
                                : 0;

                            Intent intent = new Intent(ACTION_USB_PERMISSION);
                            intent.setPackage(Emulator.getAppContext().getPackageName());

                            PendingIntent permissionIntent = PendingIntent.getBroadcast(
                                Emulator.getAppContext(),
                                0,
                                intent,
                                flags
                            );

                            usbManager.requestPermission(usbDevice, permissionIntent);
                        });
                    }
                }
            }
        }
    }

    //! Returns a list of InputDevice IDs which have the given VID and PID
    private int[] getKnownDeviceIdsByVidPid(int vendorId, int productId) {
        List<Integer> matches = new ArrayList<>();

        for (int id : knownDevices) {
            InputDevice device = InputDevice.getDevice(id);
            if (
                device != null &&
                device.getVendorId() == vendorId &&
                device.getProductId() == productId
            ) {
                matches.add(id);
            }
        }

        int[] result = new int[matches.size()];
        for (int i = 0; i < result.length; i++) {
            result[i] = matches.get(i);
        }

        return result;
    }

    //! @return true if the given VID and PID is a "priority device" and should be enumerated upon attachment
    private boolean isPriorityDevice(int vendorId, int productId) {
        // At the moment, devices which have priority are 1:1 with devices that require permission
        return isPermissionRequired(vendorId, productId);
    }

    public native void init();
    public native void virtualReleaseAll();
    public native void virtualJoystick(float x, float y);
    public native void virtualButtonInput(int key, boolean pressed);
    private native boolean joystickButtonEvent(int id, int button, boolean pressed);
    private native boolean joystickAxisEvent(int id, int button, int value);
    public native void mouseEvent(int xpos, int ypos, int buttons);
    public native void mouseScrollEvent(int scrollValue);
    public native void touchMouseEvent(int xpos, int ypos, int buttons);
    private native boolean isPermissionRequired(int vendorId, int productId);
    private native void joystickAdded(UsbManager usbManager, int id, int maple_port);
    private native void joystickRemoved(int id);
    private native void permissionGranted(UsbManager usbManager, int[] ids);
    public native boolean keyboardEvent(int key, boolean pressed);
    public native void keyboardText(int c);
    public static native boolean isMicPluggedIn();
}
