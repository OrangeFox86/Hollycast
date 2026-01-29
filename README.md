
**Hollycast**


# Hollycast

Hollycast is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator, derived of [Flycast](https://github.com/flyinghead/flycast), the open-source Sega Dreamcast emulator.
Our mission is simple: feasibly **follow upstream** while delivering a **more UI-friendly**, **feature-rich**, and **power-user capable** experience.

## Philosophy

### Upstream-first
We aim to follow Flycast as much and regularly pull upstream changes. We will continue to do this until it becomes infeasible to do so. Hollycast exists to:
- Reduce friction for users (better UX, clearer settings, sane defaults)
- Experiment with features that may not fit upstream’s goals
- Ship improvements that the community has been requesting
- Allow a little bit more freedom to developers, *while* maintaing high standards to ensure **continued stability**

Fun will be our filter; if it's fun, do it! Developers run on passion for their craft. This should be encouraged whenever possible.

### Performance & Accuracy
We treat performance drops and decreased accuracy as bugs. While any software can regress during development, our goal is:
- **Match or exceed Flycast performance**
- **Improve accuracy where possible**
- While fun is our filter, changes should always be fully tested against real-world performance before release in order to prevent issues, regressions, or performance degredation.

### Features that didn’t make the cut
Flycast is intentionally selective about what gets merged. Hollycast is where we can:
- Explore requested features and select based on what users care about rather than maintainability
- Add optional enhancements behind toggles when appropriate
- Make large and experimental changes that Flycast won't allow
- Bring some things up to date, as the emulator is getting up there in age, and some parts have not seen much attention.

## What’s different?
Hollycast is focused on:
- A **new, more modern UI**
- More quality-of-life features (especially for daily users)
- Advanced controls for enthusiasts without overwhelming new users
- Much more "user Friendly" and will allow more of a "Grab and Go" experece.
- Better Documentation of what settings do and when they need to bee used.
- Made the settings more of a "Instal and Run" setup, where Power Users still can access all the settings,but New users, wont be overwhealmed and can hit the ground running.


## Compatibility
Hollycast is based on Flycast, and aims to support the same platforms and releases where possible. If Flycast can do it, Hollycast will too — and ideally with an even smoother experience.

> Note: Hollycast is community-driven and under active development. Expect rapid iteration, and please report issues with logs and steps to reproduce. If you would like to contribute, Please join the discord  open up a Feature Request with your plans, so it can be discussed first.





### Build instructions:
```
$ git clone --recursive https://github.com/OrangeFox86/hollycast.git
$ cd flycast
$ mkdir build && cd build
$ cmake ..
$ make
```
