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

We want users to be able to connect/disconnect DreamLinks in the middle of a game with minimal disruption. For example, if a connection is dropped due to an error, like an external program crashing, etc., there should be a straightforward way of re-establishing the connection. For example, an automatic reconnect mechanism or UI command.

### Configuration

DreamLinks with associated physical controllers can be used by selecting certain controller types for the port in the Input UI. For example, the controller in port A, could be set to DreamPicoPort, which means that the state and behavior of the controller and expansion slots for that port, is determined by the behavior of the DreamPicoPort device connected via a DreamLink.

DreamPotato can be used in a given expansion slot by choosing it as the expansion device type in a given slot. For example, instead of using a VMU or Vibration Pack in a given slot, a DreamPotato can be selected instead. This means that the state and behavior of that slot is determined by a DreamPotato instance connected via a DreamLink. This includes: is any device connected to the slot at all (which can change over time), and, what are the contents/state of the device in the slot.

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

We determine whether the external VMU state changed in the following way:
- Track a "blockRead" flag for each block of the VMU. Default all to false when the VMU is connected. Set the flag to true whenever a given block of the VMU is used. (The VMU image has 256 blocks.)
- Mirror the contents of the blocks which were used. Persist this data in the save state.
- When a save state is loaded, then for each block that has been read during the session, read the corresponding block from the external VMU.
- If the contents of all the external VMU blocks are equal to the contents of the mirrored blocks, then, the external VMU is assumed to not have changed, and doesn't need to be reconnected.

Effectively, all we are doing with this check, is upholding the assumption held by the emulated game, that the contents of the blocks it used have not changed since the last use. We don't care about the contents of blocks the game "has not used yet", or perhaps that the game will never use.

#### Alternatives

Reloading Maple devices during gameplay can cause disruptive behavior in certain games. See https://github.com/flyinghead/flycast/issues/2013#issuecomment-3074306286. Therefore, it may be preferable to avoid a solution which involves automatically reloading Maple devices.

Because of this, we could consider disabling and loading state in certain scenarios involving DreamLinks. For example, disabling only when physical VMU memory is being used. However, save states are a fundamental quality of life feature in emulators, which users want to be able to use in a variety of conditions. Forcing the user to choose between using physical memory or using save states, or to constantly dig thru the UI and switch the setting back and forth depending on which they want to use at a given moment, is quite painful.

We could also consider implicitly disconnecting the physical memory when loading a state, if we stored all the contents of the external VMU at the time of saving the state. However, it may be the case that a user who is loading state, nevertheless doesn't wish to *stop* using physical memory just because of this, and, by disconnecting the physical memory, we are forcing them to dig thru the UI, or manually disconnect/reconnect controllers, etc. anyway.

### Messaging

A DreamLink implementation is expected to be able to handle the standard set of message types for the peripherals it reports usage of to Hollycast. See https://dreamcast.wiki/Maple_bus#Payload for detailed definitions of specific message types.

### Transport

Each of the device-specific DreamLink implementations allows use of a different transport, such as TCP or USB, while allowing the Maple layer of Hollycast to communicate with each in a mostly uniform way.

#### DreamConn transport

For DreamConn controller connections, the DreamConn Utility starts a local TCP server on a known set of 4 ports (37393-37396, inclusive), each corresponding to a Dreamcast controller port.

Hollycast writes Maple messages to the port in an "ASCII hex lines" format. For example, see the [GetCondition](https://dreamcast.wiki/Maple_bus#Get_Condition_Payload_Structure_(cmd_0x09)) message, which is sent to ask the peripheral to enumerate the attached devices:
`09 20 00 01 00 00 00 01\r\n`

The following is a byte-by-byte breakdown of this message, to serve as an example. See also [Maple Bus Data Packet](https://dreamcast.wiki/Maple_bus#Maple_Bus_Data_Packet) documentation.
- `09` is the command `GetCondition`.
- `20` indicates the recipient address is the main peripheral (the controller).
- `00` is the sender address (the console).
- `01` is the length of the rest of the message.
- `00 00 00 01` is the `Controller` function type.

The recipient is expected to reply to this message in "ASCII hex lines" format. e.g. a valid reply to `GetCondition` would be an `Ack` message similar to:  
`07 00 01 00\r\n`
- `07` is the command `Acknowledge`
- `00` is the recipient address
- `01` in the sender address indicates some device is connected to slot 1, and no device is connected to slot 2.
- `00` is the length of the rest of the message.

Note that a reply is not expected for the following message types:
- Updating the VMU screen (Command `WriteBlock` with function `Screen`)
- Updating the VMU buzzer (Command `SetCondition` with function `Clock`)
- Updating the jump pack state (Command `SetCondition` with function `Vibration`)

If the DreamConn Utility is sent a message in an unexpected format, it will reply with a string error message, prefixed with `ERROR`, such as:  
`ERROR: Unsupported function code 0x42.\r\n`

#### DreamPotato transport

Currently, DreamPotato uses the same format and set of TCP ports as DreamConn. This may change in the future, to e.g.:
- use a different set of ports
- use configurable ports
- write binary data instead of ASCII hex lines
- use extra custom message type(s) to allow client to discover the server capabilities upon connection and respond accordingly
- or other changes

#### DreamPicoPort transport

DreamPicoPort uses libusb to communicate with the Raspberry Pi Pico through the [DreamPicoPort-API](https://github.com/OrangeFox86/DreamPicoPort-API). All communication at the API is event-based.
