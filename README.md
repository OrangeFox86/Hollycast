# Hollycast

Hollycast is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator, derived from [Flycast](https://github.com/flyinghead/flycast), the open-source Sega Dreamcast emulator.

Our mission is simple: follow upstream core emulation code while delivering a modern UI, more front-end features, and power-user controls.

## Mission & Philosophy

Hollycast tracks the Flycast core while providing a home for features and experiments that fall outside the upstream scope. The main reasons for this fork are to:

- Unlock functionality: We integrate community-requested features and experimental changes that Flycast chooses not to merge.
- Developer freedom: We provide a stable sandbox for passion features and modernizations that keep the emulator evolving.

## Technical Standards

We are committed to maintaining the high bar for speed and precision set by the Flycast core.

- Performance parity: We aim to match or exceed the performance benchmarks of the current Flycast core.
- Verified accuracy: We use the core codebase's internal test suite alongside our own growing collection of tests to prevent regressions.
- Core integrity: New features are built on a rock-solid foundation, ensuring that fun never comes at the cost of stability.

## Key Differences

- Modern interface: A reimagined UI designed for clarity.
- Out-of-the-box optimization: Sane defaults allow new users to grab and go, while keeping advanced controls accessible for power users.
- Quality of life: Frequent updates to aged components and new features requested specifically by the daily-player community.
- Pro gamer ready: Already playing many titles better than original hardware, you can aim for a vanilla experience or enjoy the extra enhancements that help you get the most out of your sessions.
- Continued development updates: As Hollycast grows, bringing in updates from Flycast will get harder. The goal is to carry forward the performance improvements, enhancements, and features that fit Hollycast without introducing regressions.

## Compatibility & Contribution

Hollycast aims to support every platform and release offered by Flycast. If Flycast can do it, Hollycast will too, ideally with a newer looking and more feature rich environment while keeping the performance you have come to know and love from Flyinghead's hard work and dedication on Flycast.

> **Note:** Hollycast is under active development. If you'd like to contribute, please [join our Discord](https://discord.gg/pYVqGqvFJW) and open a Feature Request to discuss your plans before submitting code.

### Build instructions:
The following assumes prerequisites are already installed. Refer to the [c-cpp CI file](https://github.com/OrangeFox86/Hollycast/blob/master/.github/workflows/c-cpp.yml) under `Set up build environment` for your specific operating system.

```bash
# Clone repo
$ git clone --recursive https://github.com/OrangeFox86/hollycast.git
$ cd hollycast

# Update submodules (this needs to be manually performed when submodule versions are updated)
$ git submodule update --init --recursive

# Configure the build (make sln file, etc., depending on your platform)
$ cmake -B build-debug/

# Run the build
$ cmake --build build-debug/
```
