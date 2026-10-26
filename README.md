# Visual Pinball

[![Sponsor](https://img.shields.io/badge/Sponsor-SgtBilko76-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/SgtBilko76)


## Standalone VR port for Meta Quest and PICO

This fork builds Visual Pinball X as a **standalone VR app** for **Meta Quest** (tested on Quest 3) and **PICO** headsets (OpenXR, Vulkan). No PC, no streaming: tables and ROMs live on the headset.

**[Download the latest release](https://github.com/SgtBilko76/VPinball-VR/releases)** - more ports at **[portroyale.online](https://portroyale.online)**.

- **Table downloader** (Quest): pick tables from the Visual Pinball Collection on archive.org, ROM and backglass are installed with them
- Main menu on a curved screen in VR, left stick click switches between mixed reality and a black VR space
- Mixed reality (passthrough) switchable from the in-game menu
- Backglass rebuilt from the desktop backdrop for tables not designed for VR, including EM score reels
- VR room switching, adjustable table position, angle, size and distance
- Standard DMD and segment score displays for tables without their own 3D display

Table script overrides that switch on the VR room of tables shipping with it disabled are in [quest-overrides](quest-overrides).

Everything below is the upstream Visual Pinball documentation.

*An open source pinball table editor and simulator.*

This project was started by Randy Davis, open sourced in 2010 and continued by the Visual Pinball development team. This is the official repository.

## Features

- Simulates pinball table physics and renders the table with DirectX, OpenGL or [bgfx](https://bkaradzic.github.io/bgfx/overview.html)
- Simple editor to (re-)create any kind of pinball table
- Live editing of most content within the rendered viewport
- Table logic (and game rules) can be controlled via Visual Basic Script
- Over 1050 real/unique pinball machines from ~100 manufacturers, plus over 550 original creations were rebuilt/designed using the Visual Pinball X editor (over 3000 if one counts all released tables, incl. MODs and different variants), and even more when including its predecessor versions (Visual Pinball 9.X)
- Emulation of real pinball machines via [PinMAME](https://github.com/vpinball/pinmame) is possible via Visual Basic Script (Visual PinMAME), or via the libPinMAME-API/plugin
- Supports configurable camera views (e.g. to allow for correct display in virtual pinball cabinets)
- Support for Tablet/Touch input, Joypads, or specialized pinball controllers
- Support for Stereo3D output
- Support for Head tracking via BAM
- Support for VR/XR HMD rendering (including [PUP](https://www.nailbuster.com/wikipinup), [B2S](https://github.com/vpinball/b2s-backglass) backglass and DMD output support)
- Support for WCG/HDR rendering (for now only via the BGFX (D3D11/12) build)
- Support for Windows (x86), Linux (x86/Arm, incl. RaspberryPi and RK3588), macOS, iOS/tvOS, Android (the latter builds are also available via the respective app stores for free)
- Plugin system to drive/fuel all kinds of displays (DMD, backglass, etc), add custom/dynamically-changed content (PUP, Serum, etc), direct output framework (DOF), sensors, and much more (WIP)

## Download

All releases are available on the [releases page](https://github.com/vpinball/vpinball/releases).

## Documentation

Documentation is currently sparse. Check the [docs](docs) directory for various guides and references.

An [unofficial wiki](https://github.com/dekay/vpinball-wiki) is currently being developed. Community contributions, suggestions, and help are welcome to improve the resource for all users.

## How to build

Build instructions are available in the [make directory README](make/README.md).

## Participate

Read through our [contribution](CONTRIBUTING.md) rules carefully before opening PRs, please.
