# Install guide (the easy version)

This repository ships **no game files and no ready-made game binary**: the game code is converted on *your*
PC from *your* copy of the game. You need the retail PC game (CD / ISO), a New 3DS and about 15 minutes.

Checklist: [1. Check your game](#1-check-your-game) -> [2. Install the tools](#2-install-the-tools-once) ->
[3. Build](#3-build) -> [4. Copy to the SD card](#4-copy-to-the-sd-card) -> [5. Start](#5-start-it)

## 1. Check your game

Tested with the English retail CD (*Star Wars Episode I: The Phantom Menace*, PC, 1999). Get the files from
the disc or the ISO: on Windows 10/11 double-click the `.iso` to mount it as a drive (or open it with 7-Zip).

The converter depends on exact code addresses, so your `GAMEDATA\BIN\WMAIN.EXE` must match
(PowerShell: `Get-FileHash -Algorithm SHA1 WMAIN.EXE`):

| File | Size | SHA-1 |
|---|---|---|
| `WMAIN.EXE` | 829,952 | `49d57871b57278270af929dcdf6e3f7ccf3b1adc` |
| `BINKW32.DLL` | | `8dc84b70f15c3de093bbeddb5f0ea58fdca9090b` |
| `IMUSE.DLL` | | `589f25d77fa2bf5ed15d92def65692e0e9931a40` |

Other versions or languages are untested and will most likely not work.

## 2. Install the tools (once)

1. **devkitPro** with the 3DS toolchain: <https://devkitpro.org/wiki/Getting_Started> (Windows installer,
   tick "3DS Development"; libctru and citro3d are included).
2. **Python 3** (<https://www.python.org>), then in a terminal: `pip install capstone`.

## 3. Build

Copy the whole `GAMEDATA` folder of the disc somewhere on your PC (e.g. `C:\tpm\GAMEDATA`). Then, in a
terminal in the folder of this repository:

```sh
python tools/lifter/lift_all.py C:/tpm/GAMEDATA source/gen
```

This reads your `WMAIN.EXE` and the two DLLs and writes C code into `source/gen` (takes a while;
this folder is git-ignored and must never be uploaded or shared).

Then open the **devkitPro MSYS2 shell** (Start menu: "MSYS2" under devkitPro), go to the repository and run:

```sh
make
```

Result: `tpm3ds.3dsx`. (Optional CIA for the HOME menu: `sh tools/cia/build_cia.sh`, needs `makerom`.)

## 4. Copy to the SD card

Everything lives in one folder, `sdmc:/3ds/tpm/`:

```
SD card
└── 3ds
    └── tpm
        ├── tpm3ds.3dsx          <- the file you just built
        └── data
            ├── BIN              <- the 7 folders from the disc's GAMEDATA
            ├── ETC
            ├── GOBS
            ├── JCG
            ├── LEVEL
            ├── MOVIE
            └── MUSIC
```

In other words: open `GAMEDATA` on the disc / ISO, select **everything inside it**, and copy it into
`SD:\3ds\tpm\data\`. Do not copy `INSTALL`, `TPM.EXE` or `AUTORUN.INF`.

You need about **1 GB** free on the SD card (590 MB of game data plus 120 MB that the game unpacks on the first start).
If you installed the CIA instead of using the 3dsx, the data folder is the same.

## 5. Start it

- Homebrew Launcher: start **tpm3ds**.
- The **first start** unpacks `GOBS/BIG.Z` (it shows a percentage on the bottom screen). This takes a few minutes
  and happens only once. Do not close the app.
- After that the intro movies play, then the menu. The bottom screen has tabs for brightness, cheats and
  save slots (quick save / load).

Saves and the log file (`log.txt`) are written to `sdmc:/3ds/tpm/`. If something does not work, the end of
`log.txt` says why.

## Troubleshooting

| Problem | Fix |
|---|---|
| `lift_all.py` stops with an error | Wrong game version: compare the SHA-1 values above. Also check `pip install capstone`. |
| `make` complains about `DEVKITARM` | Use the devkitPro MSYS2 shell, not a normal terminal. |
| Black screen / back to the launcher | Check that `data/BIN/WMAIN.EXE` exists under `sdmc:/3ds/tpm/data/`. Read `log.txt`. |
| First start seems stuck | The unpacking takes several minutes; wait for the percentage to reach 100. |
| "Needs a New 3DS" | Only the New 3DS / New 2DS XL are supported; the original 3DS / 2DS is too slow. |
