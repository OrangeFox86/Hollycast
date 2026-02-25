# Hollycast

Hollycast is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator, derived of [Flycast](https://github.com/flyinghead/flycast), the open-source Sega Dreamcast emulator.
Our mission is simple: **follow upstream** core emulation code while delivering a **modern UI**, **more front-end features**, and **power-user controls**.

## 🚀 Mission & Philosophy
Hollycast tracks the **Flycast** core while providing a home for the features and experiments that fall outside the upstream scope. We operate on a simple principle: **Fun is the filter.** When developers follow their passion, the community wins.

* **Unlock Functionality:** We integrate community-requested features and experimental changes that Flycast chooses not to merge.
* **Developer Freedom:** We provide a stable sandbox for "passion features" and modernizations that keep the emulator evolving.

## 🛠️ Technical Standards
We are committed to maintaining the high bar for speed and precision set by the Flycast core.

* **Performance Parity:** We aim to match or exceed the performance benchmarks of the current Flycast core.
* **Verified Accuracy:** We utilize the core codebase’s internal test suite alongside our own growing collection of tests to prevent regressions.
* **Core Integrity:** New features are built on a rock-solid foundation, ensuring that "fun" never comes at the cost of stability.

## ✨ Key Differences
* **Modern Interface:** A reimagined UI designed for clarity.
* **Out-of-the-box Optimization:** Sane defaults allow new users to "grab and go," while keeping advanced controls accessible for power users.
* **Quality of Life:** Frequent updates to aged components and new features requested specifically by the daily-player community.

## 📦 Compatibility & Contribution
Hollycast aims to support every platform and release offered by Flycast. If Flycast can do it, Hollycast will too, ideally with a smoother experience.

> **Note:** Hollycast is under active development. If you’d like to contribute, please [join our Discord](https://discord.gg/pYVqGqvFJW) and open a Feature Request to discuss your plans before submitting code.

### Build instructions:
```
$ git clone --recursive https://github.com/OrangeFox86/hollycast.git
$ cd flycast
$ mkdir build && cd build
$ cmake ..
$ make
```
