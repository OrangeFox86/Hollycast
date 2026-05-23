# DreamPotato Integrated Mode

We want to make Hollycast a "one stop shop" for emulating the Dreamcast and VMU together.

The goal is that a user can just launch Hollycast, enable this mode, and have the VMU emulator appear, and use the correct VMU file automatically, based on what Hollycast is doing. This removes the need to manually launch DreamPotato, manually select a file for the particular game, and so on.

We refer to this functionality as "integrated mode". The existing feature where both DreamPotato and Hollycast are launched manually, is called "standalone mode".

## New command line flags

We add the following command line flags to DreamPotato:

- `--integrated`: indicates the integrated mode should be used in DreamPotato. This disables a number of UI commands and settings. Mainly, commands related to managing VMU files.
- `--port=A`: indicates the port letter that the DreamPotato instance should use. Any port letters A-D are permitted.
- `--slots=1`: indicates which slots for the port contain "DreamPotato VMUs". Slots `1`, `2`, `both` are permitted.

When the integrated mode is enabled in Hollycast, then, Hollycast will launch a DreamPotato subprocess at the appropriate time, passing the appropriate values for the above flags.

## Port selection

Today, the DreamPotato server uses a hard-coded set of local ports for standalone mode. This means it's not possible to have multiple Hollycast sessions, operating alongside DreamPotato on the same ports, on the same machine.

With integrated mode, we could have DreamPotato start listening on a free port number automatically and send it back to the parent Hollycast process somehow. For example, via its standard output stream.

This is not yet implemented, and, would probably be controlled by an additional new DreamPotato flag.

## File selection

We essentially want the "VMU file used by Hollycast", to be automatically provided to DreamPotato.

This is accomplished when first launching DreamPotato, by simply passing it the path of the VMU file as it already supports today.

However, the VMU path can change depending on the game (Per-game VMU A1). We don't want to re-launch the DreamPotato process when changing games as this is disruptive and slow.

To support this, we add the following Maple pseudo-commands:
- `Open File`: Contains a file path as the payload. DreamPotato will receive this message and will open the given file using the "addressed" VMU.
    - Command code: 0xC0.
    - Since Maple payloads are 32-bit words, the file path may be zero-padded at the end. DreamPotato will strip the trailing zeros off of the path before using it.
    - The file path is assumed to be encoded in UTF-8.

## Save states

We aren't doing anything special with this right now, compared to the existing DreamPotato connectivity.

For example, loading a save state in this mode, will not revert the VMU contents to when the state was saved, like would be done for ordinary VMUs.

Theoretically, this tighter integration, would be an opportunity to ensure that the DreamPotato savestate is strongly coupled to the Hollycast savestate. This would allow restoring the VMU contents to match the savestate under all conditions, and prevent a need to "simulate reconnecting" VMUs. However, this is additional work and not thought to be part of the "most critical path" to get working.

One way we could approach restoring the state of the device is:
- If the DreamPotato VMU was docked when the state was saved, then, include the flash memory contents in the savestate. When the savestate loaded, perform the following steps:
    1) Tell DreamPotato to close the VMU file it has open.
    2) Overwrite the VMU file.
    3) Tell DreamPotato to reopen the VMU file and dock the VMU (no-op if the VMU is already docked).
    4) Send a Write LCD message with the VMU screen data from the savestate.
- If the DreamPotato VMU was *ejected* when the state was saved, then, don't include anything about the VMU in the savestate.
    - When loading the state, the only thing we want to do is tell DreamPotato to eject the VMU (which is a no-op if VMU is already ejected).
