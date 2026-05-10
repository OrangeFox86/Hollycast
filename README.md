# HolyCast

*Trusted Core. Modern Features.*

## What Is It?

**HollyCast** is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator derived from [**Flycast**](https://github.com/flyinghead/flycast).

As technology has improved, emulators have evolved with new and exciting features that modern devices can support, making the overall user experience even better. **HollyCast** brings users everything they know and love from Flycast, while adding a full feature suite designed for next-level emulation.

With support for real hardware integration, the ability to play as if you were using an original console, and an integrated VMU emulator for playing VMU mini-games, **HollyCast** helps bridge the gap between emulation and real hardware.

At the same time, we have made emulation simpler for new users with settings presets, a redesigned UI, and clearer explanations that make each setting easier to understand and use.


<img src="shell/linux/flycast.png" alt="flycast logo" width="150"/>



Information about configuration and supported features can be found on [**TheArcadeStriker's flycast wiki**](https://github.com/TheArcadeStriker/flycast-wiki/wiki).

Join us on our [**Discord server**]([https://discord.gg/X8YWP8w](https://discord.gg/bzj4DWsmBn)) for a chat. 

## Install 

### Android ![android](https://flyinghead.github.io/flycast-builds/android.jpg)
Install Flycast from [**Google Play**](https://play.google.com/store/apps/details?id=com.flycast.emulator).
### Flatpak (Linux ![ubuntu logo](https://flyinghead.github.io/flycast-builds/ubuntu.png))

1. [Set up Flatpak](https://www.flatpak.org/setup/).

2. Install Flycast from [Flathub](https://flathub.org/apps/details/org.flycast.Flycast):

`flatpak install -y org.flycast.Flycast`

3. Run Flycast:

`flatpak run org.flycast.Flycast`

### Homebrew (MacOS ![apple logo](https://flyinghead.github.io/flycast-builds/apple.png))

1. [Set up Homebrew](https://brew.sh).

2. Install Flycast via Homebrew:

`brew install --cask flycast`

### iOS

Due to persistent harassment from an iOS user, support for this platform has been dropped. 

### Xbox One/Series ![xbox logo](https://flyinghead.github.io/flycast-builds/xbox.png)

Grab the latest build from [**the builds page**](https://flyinghead.github.io/flycast-builds/), or the [**GitHub Actions**](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml). Then install it using the **Xbox Device Portal**.


Get fresh builds for your system [**on the builds page**](https://github.com/OrangeFox86/Hollycast/releases).

**New:** Now automated test results are available as well. 

### Build instructions:
```
$ git clone --recursive https://github.com/flyinghead/flycast.git
$ cd flycast
$ mkdir build && cd build
$ cmake ..
$ make
```
