# DreamLink

## Summary

DreamLink is an interface in Hollycast which allows it to communicate with external Maple expansion devices.

Example devices which use the DreamLink interface:
- DreamConn controllers
- DreamPicoPort controllers
- DreamPotato (VMU emulator)

## Motivation

- Real Dreamcast controllers can be used with Hollycast. As part of this, we want the physical expansion devices which connect to the controller to "just work" in Hollycast in a similar way as they would with a real Dreamcast.
- We want Hollycast and DreamPotato (or potentially other VMU emulators) to be able to connect to each other directly to support an experience similar to using a real Dreamcast and VMU together.

## Detailed design

### Connection

DreamLink implementations all expose a connection state, which is displayed in an appropriate spot in UI, to make it clear to the user whether the external devices are actually being used or not, and whether their hardware/software setup is working as expected.

DreamLinks associated with physical controllers generally handle setting up and tearing down the DreamLink connection, by checking for specific device IDs when an SDL controller is being set up. Note that this means the controller-based DreamLinks have a dependency on SDL, which means they only work on the desktop OS targets and not on Android, Switch, etc.

If a controlled-based DreamLink connection is disrupted, the user generally needs to disconnect and reconnect the controller or restart Hollycast to re-establish the connection.

The DreamPotato case works by the user ensuring both Hollycast and DreamPotato are running, then starting a game. If the connection is disrupted, the user can reconnect by pausing and using a reconnect command in the UI.

### Configuration

DreamLinks with associated physical controllers can be used by selecting certain controller types for the port in the Input UI. For example, the controller in port A, could be set to DreamPicoPort, which means that the state and behavior of the controller and expansion slots for that port, is determined by the behavior of the DreamPicoPort device connected via a DreamLink.

DreamPotato can be used in a given expansion slot by choosing it as the expansion device type in a given slot. For example, instead of using a VMU or Vibration Pack in a given slot, a DreamPotato can be selected instead. This means that the state and behavior of that slot is determined by a DreamPotato instance connected via a DreamLink. This includes: is any device connected to the slot at all (which can change over time), and, what are the contents/state of the device in the slot.

### Transport

Each of the device-specific DreamLink implementations allows use of a different transport, such as TCP or USB, while allowing the Maple layer of Hollycast to communicate with each in a mostly uniform way.

At time of writing, DreamConn controllers and DreamPotato both start a local TCP server on the same set of ports, which Flycast connects to. A different TCP port is reserved for each controller port.

DreamPicoPort uses libusb to communicate with the Raspberry Pi Pico.

### Graceful behavior

Generally, it is desirable for Hollycast and the DreamLink implementations to behave gracefully in a variety of conditions which could arise during real use:
- Connecting or disconnecting physical controllers with associated DreamLinks during gameplay.
- Connecting or disconnecting external expansion devices during gameplay.
- Connection failures in Hollycast or in DreamLink implementation itself, e.g. a TCP connection being closed due to DreamPotato crashing.

When determining what Hollycast should do under some particular conditions, it is best to start by determining what an analogous situation involving real Dreamcast hardware would be, and imitating as best as possible what would occur for that.

### Physical memory access

A DreamLink implementation can indicate support for memory access on external VMUs. At time of writing, DreamConn doesn't support this, but DreamPicoPort and DreamPotato do. The user setting "Use Physical VMU Memory" controls whether to use the physical memory, or to use Flycast's built-in memory card files instead (similarly to if a DreamLink were not being used at all).

When the DreamLink supports physical memory, and the user enables the option, then VMU read/write messages will be forwarded through the DreamLink and fulfilled by the external device.

### Save states

Saving and loading states when DreamLinks are in use poses a challenge in general, especially when physical memory is being used. Typically, Hollycast includes the configuration and state of emulated controllers and expansion devices in save states. (Aside: This is a significantly different behavior than other major emulators for game consoles with removable memory cards, such as Dolphin, DuckStation, or PCSX2. Those emulators by default will simulate removing and re-inserting memory cards upon loading a state. It is an unfortunate quirk of the Dreamcast that some games do not handle this gracefully during normal gameplay.)

A DreamLink device by nature represents an external expansion device which Hollycast doesn't have control over. It's difficult to restore its state in general, especially when a state difference might, for example, represent a physical device being connected to a slot or not.

Hollycast will address this issue by ensuring that when loading state, if any expansion slots are using DreamLinks with physical memory (i.e. DreamPotato expansion devices, or DreamPicoPort controllers), and the state of the external device(s) may have changed since saving the state, then the emulated Maple devices are reloaded automatically.

There are various possible ways to determine if the external devices may have changed. One simple way would be to track a timestamp whenever Maple devices are changed or a WriteBlock message is sent to a VMU. Then, compare this timestamp to the last write time of a save state file. If the save state write time is older than the Maple device update time, then, expansion devices need to be reloaded. (Finer-grained methods of detecting possible changes also exist, but this suggestion is included as a starting point.)

#### Alternatives

Reloading Maple devices during gameplay can cause disruptive behavior in certain games. See https://github.com/flyinghead/flycast/issues/2013#issuecomment-3074306286. Therefore, it may be preferable to avoid a solution which involves automatically reloading Maple devices.

Because of this, we could consider disabling and loading state in certain scenarios involving DreamLinks. For example, disabling only when physical VMU memory is being used. However, save states are a fundamental quality of life feature in emulators, which users want to be able to use in a variety of conditions. Forcing the user to choose between using physical memory or using save states, or to constantly dig thru the UI and switch the setting back and forth depending on which they want to use at a given moment, is quite painful.

We could also consider implicitly disconnecting the physical memory when loading a state, if we stored all the contents of the external VMU at the time of saving the state. However, it may be the case that a user who is loading state, nevertheless doesn't wish to *stop* using physical memory just because of this, and, by disconnecting the physical memory, we are forcing them to dig thru the UI, or manually disconnect/reconnect controllers, etc. anyway.

### Messaging

A Maple message is generally categorized by its *type* and *function*. Examples of message *types* include GetDeviceInfo, GetCondition, etc. Examples of message *functions* include Input, Storage, LCD, etc.

On real hardware, every Maple message expects a response, and, every message is sent by the Dreamcast and the response is sent by the appropriate peripheral (the controller itself or an expansion device).

This section lays out the set of messages which a DreamLink implementation is expected to be able to handle. The sending message types will be named in *Type, Function* form.

The exact binary form of the messages isn't specified here currently, but, it is documented elsewhere and can generally be found in the implementation.

#### GetCondition, Input

GetCondition, Input is a request to know which expansion slots contain expansion devices. The DreamLink is expected to respond with *message type* Ack, which sets the appropriate bits in the Maple sender address field, to indicate this information.

As can occur on real hardware, it is expected that the devices in expansion slots could be inserted or removed at any time. Hollycast will account for this by polling all the DreamLinks in use with this message every frame.

Generally, the bits in the reply must change if there is a change to the specific expansion devices being used. This means that, for example, if a DreamLink wants to change which VMU is currently being used (for example, the DreamPotato user docks the VMU, then uses Open to load a different VMU file), then it must first reply to GetCondition, Input with the appropriate bits cleared, and then reply with the bits set on the next frame, to effectively signal to Hollycast that a device was removed and then another device was inserted.

#### GetDeviceInfo

Queries for the specific type of device in an expansion slot. The expected response is of type DeviceInfoTransfer and sets the appropriate Maple *function* flags depending on the device being used. For example, an ordinary VMU would set the `Storage | LCD | Clock` function flags, while a jump pack would set just the Vibration flag. TODO: what function does the sender use for this?

#### SetCondition, Clock

Plays a sound on the VMU speaker. This is a PWM sound, i.e. a simple square wave. The parameters to the message indicate the period and duty cycle of the sound. The sound should continue to be played until a new SetCondition, Clock message is received.

It is recommended that a DreamLink implementation stops playing sound automatically if it detects the connection is disrupted.

TODO: should Hollycast automatically send messages to stop/start the "current sound" when the emulator pauses/resumes? Otherwise, if the user pauses while a sound is playing on the VMU, the sound might keep playing indefinitely.

#### WriteBlock, Clock

Updates the real-time clock on a VMU. TODO: I don't recall if we actually send this or not.

#### WriteBlock, LCD

Updates the contents of the VMU screen. It is recommended that a DreamLink implementation clears the screen if it detects the connection is disrupted.

#### ReadBlock, Storage

Reads a 512-byte block of the VMU's flash storage. Expects a DataTransfer response including the contents of the block. Only sent when *physical memory access* is enabled.

#### WriteBlock, Storage

Writes a block of the VMU's flash storage. This message generally comes in sequences of 4, distinguished by a *phase* field, which each write 128 bytes of the block. Expects an Ack response. Only sent when *physical memory access* is enabled.

#### CompleteWrite, Storage

Indicates completing a write of a 512-byte block of the VMU's flash storage. Expects an Ack response.
