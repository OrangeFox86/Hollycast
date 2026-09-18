# Hollycast

![Hollycast](banner.png)

<sub>Logo credit: [@ruva](https://artistree.io/ruva)</sub>

Hollycast is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator, derived from [Flycast](https://github.com/flyinghead/flycast), the open-source Sega Dreamcast emulator.

## Mission & Philosophy

Hollycast tracks the Flycast core while providing a home for features and experiments that fall outside the upstream scope. The main reasons for this fork are to:

- Unlock functionality: We integrate community-requested features and experimental changes that Flycast chooses not to merge.
- Developer freedom: We provide a stable sandbox for passion features and modernizations that keep the emulator evolving.

## Technical Standards

We are committed to maintaining the high bar for speed and precision set by the Flycast core.

- Performance parity: We aim to match or exceed the performance benchmarks of the current Flycast core.
- Verified accuracy: We use the core codebase's internal test suite alongside our own growing collection of tests to prevent regressions.
- Core integrity: New features are built on a rock-solid foundation, ensuring that fun never comes at the cost of stability.

## Compatibility & Contribution

Hollycast aims to support every platform and release offered by Flycast. If Flycast can do it, Hollycast will too, ideally with a newer looking and more feature rich environment while keeping the performance you have come to know and love from Flyinghead's hard work and dedication on Flycast.

> **Note:** Hollycast is under active development. If you'd like to contribute, please [join our Discord](https://discord.gg/erSUx3v4YH) and open a Feature Request to discuss your plans before submitting code.

### Build Prerequisites for Windows

1. Install Visual Studio with MSVC and C++ CMake tools (under `Desktop development with C++` within the installer)
2. Add the location of cmake.exe to your PATH environment variable (ex: C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\)
3. Enable `Developer Mode` under System->Advanced within your Windows Settings in order to allow symlink creation without needing administrative privileges. **Warning:** this is generally considered to be a [vulnerability on Windows](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/create-symbolic-links#vulnerability), and it is currently only necessary to build for Android.

### Build Prerequisites for Linux

Run the following to install all prerequisites.

```
sudo apt-get update
sudo apt-get -y install git cmake gcc g++
sudo apt-get -y install ccache libao-dev libasound2-dev libevdev-dev libgl1-mesa-dev liblua5.3-dev libminiupnpc-dev libpulse-dev libsdl2-dev libudev-dev libzip-dev ninja-build libcurl4-openssl-dev libcdio-dev libfuse2 locales
sudo apt-get -y install libwayland-dev libdecor-0-dev libaudio-dev libjack-dev libsndio-dev libsamplerate0-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libdrm-dev libgbm-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev fcitx-libs-dev
```

### Build Prerequisites for macOS

1. Install Xcode application from app store
2. Accept licensing for Xcode and do first launch initialization
```bash
sudo xcodebuild -runFirstLaunch
```
3. Execute:
```bash
xcodebuild -downloadComponent MetalToolchain
```
4. Install Homebrew
5. Execute:
```bash
brew install cmake
brew install molten-vk
```

### Repository Setup

Run the following to pull down the repository and all submodules.

```bash
# Clone repo
git clone https://github.com/OrangeFox86/Hollycast.git
cd Hollycast

# Ensure symlinks are enabled for this project
git config core.symlinks true

# Update submodules (this needs to be manually performed when submodule versions are updated)
git submodule update --init --recursive --force
```

### Build Instructions for Windows/Linux/macOS

The following assumes prerequisites are already installed and working directory is Hollycast.

```bash
# Find the desired CMake preset for the current platform
cmake --list-presets

# Run CMake configure. Rerunning this is usually only necessary when certain files like CMakeLists.txt change.
cmake --preset <PRESET>

# Run the build. Using the same preset name as for the configure will generally work.
cmake --build --preset <PRESET>
```

### Build Instructions for Android

The Android image may be built from Windows, Linux, and macOS systems. [Android Studio](https://developer.android.com/studio/install) must be installed to build this package.

Ensure that `JAVA_HOME` and `ANDROID_HOME` are set in your environment before trying to build.

#### Build from Android Studio

Developing within Android Studio enables debug tools, streamlines the build process, and provides other useful features built into the IDE.

- Run Android Studio.
- Open directory `shell/android-studio` of this repo.
- The Hollycast project should be picked up automatically, with "Android" sidebar on the left, and devices and run configurations in the top right.
- `debug` build variant is used by default. If you want to build in release mode for better performance, then open **View > Tool Windows > Build Variants**, and select Active Build Variant `developerRelease`.
   - Note that `release` variant does production code signing. It's only intended for store publishing.

#### Build from Command Line

```bash
# Your working directory must be changed to shell/android-studio before building.
cd shell/android-studio

# Run the following to build for debug.
./gradlew assembleDebug bundleDebug --parallel
```

If the build gets stuck or encounters and error, run the following before trying again.

```bash
# Terminate all background Gradle Daemon processes started by gradle.
./gradlew --stop

# Remove all artifacts of previous build.
./gradlew clean
```

### Segmentation fault (SIGSEGV) / Access Violation (AV) handling

When debugging Hollycast, you may notice the debugger frequently breaking on memory access violations when running games. This is normal and expected. This is related to how the emulator's dynamic recompiler works and doesn't reflect any actual memory safety bug.

It's recommended you adjust your debugger or exception settings as needed for the current platform, in order to prevent disruptive breaks during debugging.

For example, with LLDB, pass the following startup command:
- `process handle -s false -n false -p true SIGSEGV`

For Android, if you hit the breakpoint for `art_sigsegv_fault` in disassembly, delete the breakpoint and resume debugging.

For VS Code C/C++ on Windows, use the following filter string on the "All Exceptions" breakpoint:
- `!0xC0000005`
