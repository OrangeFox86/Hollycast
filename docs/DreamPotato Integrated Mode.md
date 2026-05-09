# DreamPotato Integrated Mode

We want to make Hollycast a "one stop shop" for emulating the Dreamcast and VMU together.

The goal is that a user can just launch Hollycast, enable this mode, and have the VMU emulator appear, and use the correct VMU file automatically, based on what Hollycast is doing. This removes the need to manually launch DreamPotato, manually select a file for the particular game, and so on.

We refer to this functionality as "integrated mode". The existing feature where both DreamPotato and Hollycast are launched manually, is called "standalone mode".

## New command line flags

We add the following command line flags to DreamPotato:

- `--integrated`: indicates the integrated mode should be used in DreamPotato. This disables a number of UI commands and settings. Mainly, commands related to managing VMU files.
- `--port=A`: indicates the port letter that the DreamPotato instance should use. Any port letters A-D are permitted.
- `--slots=1`: indicates which slots for the port contain "DreamPotato VMUs". Slots `1`, `2`, `both` are permitted.

## File selection

We essentially want the "VMU file used by Hollycast", to be automatically provided to DreamPotato.

This is accomplished when first launching DreamPotato, by simply passing it the path of the VMU file as it already supports today.

However, the VMU path can change depending on the game (Per-game VMU A1). We don't want to re-launch the DreamPotato process when changing games as this is disruptive and slow.

To support this, we add the following Maple pseudo-commands:
- `Open File`: Contains a file path as the payload. DreamPotato will receive this message and will open the given file using the "addressed" VMU.
    - Code: ...

## Save states

We aren't doing anything special with this right now, compared to the existing DreamPotato connectivity.

Theoretically, this tighter integration, would be an opportunity to ensure that the DreamPotato savestate is strongly coupled to the Hollycast savestate. This would allow restoring the VMU contents to match the savestate under all conditions, and prevent a need to "simulate reconnecting" VMUs. However, this is additional work and not thought to be part of the "most critical path" to get working.
