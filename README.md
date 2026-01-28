# HollyCast
Hollycast is a Fork of Flycast, That is kept up to date consistantly to Flycast, but has

**Hollycast** is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator derived from [**Flycast**](https://github.com/flyinghead/flycast)..
# HollyCast

HollyCast is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator, derived of [Flycast](https://github.com/flyinghead/flycast), the open-source Sega Dreamcast emulator.  
Our mission is simple: **MATCH upstream** while delivering a **more UI-friendly**, **feature-rich**, and **power-user capable** experience.

## Philosophy

### Upstream-first
We aim to follow Flycast closely and regularly pull upstream changes. HollyCast exists to:
- Reduce friction for users (better UX, clearer settings, sane defaults)
- Experiment with features that may not fit upstream’s goals
- Ship improvements that the community has been requesting
- allow a little bit more freedom to developers, *while* maintaing high standards to ensure **NO RESRESSIONS**

### Performance & accuracy are non-negotiable
We treat performance drops and accuracy regressions as bugs. While any software can regress during development, our goal is:
- **Match or exceed Flycast performance**
- **Improve accuracy where possible**
- Validate changes with testing and real-world reports
- Never allow a change to cause issues or regressions where issues pop up, or performance is hit.

### Features that didn’t make the cut
Flycast is intentionally selective about what gets merged. HollyCast is where we can:
- Explore requested features
- Iterate faster
- Add optional enhancements behind toggles when appropriate
-Make large changes that Flycast won't allow
- Bring some things up to date, as the emulator is getting up there in age, and some parts have not seen much attention.

## What’s different?
HollyCast is focused on:
- A **new, more modern UI**
- More quality-of-life features (especially for daily users)
- Advanced controls for enthusiasts without overwhelming new users
- Much more "user Friendly" and will allow more of a "Grab and Go" experece.
- Better Documentation of what settings do and when they need to bee used.
- Made the settings more of a "Instal and Run" setup, where Power Users still can access all the settings,but New users, wont be overwhealmed and can hit the ground running.


## Compatibility
HollyCast is based on Flycast, and aims to support the same platforms and releases where possible. If Flycast can do it, HollyCast will too — and ideally with an even smoother experience.

> Note: HollyCast is community-driven and under active development. Expect rapid iteration, and please report issues with logs and steps to reproduce. If you would like to contribute, Please join the discord  open up a Feature Request with your plans, so it can be discussed first.





### Build instructions:
```
$ git clone --recursive **https://github.com/OrangeFox86/hollycast.git
$ cd flycast
$ mkdir build && cd build
$ cmake ..
$ make
```
