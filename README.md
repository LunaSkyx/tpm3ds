# tpm3ds

An unofficial port of *Star Wars Episode I: The Phantom Menace* (PC, 1999) to the **New Nintendo 3DS**.

The game's x86 code is not emulated: `WMAIN.EXE` and its two DLLs (Bink video, iMUSE music) are lifted
statically to C by `tools/lifter`, then compiled for the 3DS together with a small runtime that
re-implements the Win32, DirectDraw/Direct3D, Miles Sound System and Bink APIs the game calls
(`source/core/hle`). Rendering uses the 3DS GPU (citro3d); a few hot functions are hand-written natives.

**This repository contains no game files, no game code and no game assets.** You need your own copy of the
game. The code lifted from your `WMAIN.EXE` is generated on your machine and is git-ignored.
Not affiliated with or endorsed by Lucasfilm, Disney or Nintendo.

## Status

Playable start to finish (the whole game was completed on a New 3DS). Runs in
[Azahar](https://azahar-emu.org/) too. Known rough edges: occasional short stutters, long first
load of a level on hardware (voice lines are decoded lazily to keep it short), and the port is homebrew-grade.

## What you need

- A New 3DS (or Azahar) with custom firmware / Homebrew Launcher, or CIA installation via FBI
- The game's files from your own PC copy or disc (`GAMEDATA`, including `BIN`, `GOBS`, `LEVEL`, `MOVIE`, `MUSIC`)
- To build: devkitPro with devkitARM, libctru, citro3d; Python 3 with `capstone` (and `numpy` + `Pillow`
  only for the banner/icon scripts in `tools/cia`)

## Build

```sh
# 1. lift the game code from your own game files (writes source/gen, not tracked)
python tools/lifter/lift_all.py /path/to/GAMEDATA source/gen

# 2. build (devkitPro shell)
make                    # -> tpm3ds.3dsx and tpm3ds.elf
sh tools/cia/build_cia.sh   # optional: tpm3ds.cia (needs makerom)
```

Host-side unit tests (no devkitPro): `make -f Makefile.host test`.
`tools/azahar_test.sh` runs the 3dsx in Azahar with a scripted input file (see `tests/scenarios`).

## Install

**Windows, easy way:** unzip this repo and double-click `setup.bat`. Give it your game ISO (or `GAMEDATA` folder) and your SD card
letter; it installs the missing tools (Python, devkitPro, makerom), converts and builds the game, makes a CIA and copies
everything to the card. Details and the manual route: **[docs/INSTALL.md](docs/INSTALL.md)**.

The port expects the game data in `sdmc:/3ds/tpm/data/` and `tpm3ds.3dsx` in `sdmc:/3ds/tpm/`. On the first start it
unpacks `GOBS/BIG.Z`, which takes a few minutes. Saves and `log.txt` go to `sdmc:/3ds/tpm/`. The bottom screen has tabs
for game options (brightness), cheats and save slots (quick save / load).

## Layout

| Path | Contents |
|---|---|
| `source/core` | runtime: guest memory/CPU state, heap, native overrides, renderer, mixer |
| `source/core/hle` | Win32 / DirectX / Miles / Bink replacements |
| `source/pal` | platform layer (3DS and host) |
| `tools/lifter` | x86 → C static lifter |
| `tools/cia` | banner, icon and jingle generators, CIA build |
| `tests` | host tests and input scenarios |
| `docs` | development log and performance notes |

## Licence

The code and tools in this repository are under the [MIT licence](LICENSE). `source/core/dcl.c` is derived from
`blast.c` by Mark Adler (zlib licence, notice kept in the file); the UI font bitmaps come from Aileron (CC0).
The licence covers only the code in this repository and gives no rights to the game itself, its code, assets or
trademarks, which belong to their owners.
