# Install guide (the easy version)

This repository ships **no game files and no ready-made game binary**: the game code is converted on *your* PC
from *your* copy of the game. A script does all of it, you only answer two questions.

## What you need

- A **New 3DS / New 2DS XL** with Homebrew Launcher (an original 3DS is too slow)
- The retail PC game, English version: the **CD or the `.iso`** (about 1 GB free on the SD card)
- A Windows PC with two free programs:
  1. **devkitPro**, with "3DS Development" ticked: <https://devkitpro.org/wiki/Getting_Started>
  2. **Python 3** (tick "Add python.exe to PATH"): <https://www.python.org>

## Steps

1. Download this repository (green **Code** button -> *Download ZIP*) and unzip it.
2. Put your SD card in the PC.
3. **Double-click `setup.bat`.**
4. It asks:
   - *Path to the ISO or GAMEDATA folder*: drag your `.iso` (or the CD's `GAMEDATA` folder) into the window, press Enter.
   - *SD card drive letter*: type e.g. `E`, press Enter.
5. Wait (it converts and builds the game, which takes a while (on a fast PC roughly 10 minutes), then copies about 600 MB). When it says **DONE**, put the
   SD card in the 3DS.
6. Start **tpm3ds** in the Homebrew Launcher. The first start unpacks some data (a few minutes, with a percentage);
   after that it goes straight to the intro and menu.

That is all. The script checks that your game files are the right version, so you get a clear message if not.
Re-running it is safe (it only copies what is missing).

Without a SD card in the PC: press Enter at the drive-letter question; everything is collected in a folder
`sdcard` next to the script, and you copy its `3ds` folder to the root of the SD card yourself.

Command-line version: `powershell -File setup.ps1 -Game D:\TPM.iso -Sd E` (or `-Sd C:\some\folder`).

## Where things end up

```
SD card
└── 3ds
    └── tpm
        ├── tpm3ds.3dsx
        └── data               <- BIN, ETC, GOBS, JCG, LEVEL, MOVIE, MUSIC from the disc's GAMEDATA
```

Saves and a `log.txt` are written to `3ds/tpm/` as well. `source/gen` and `tpm3ds.3dsx` contain the converted game
code: keep them to yourself, never upload them.

## Manual way (if you do not want the script)

Compatible game files (English retail CD):

| File | SHA-1 |
|---|---|
| `GAMEDATA\BIN\WMAIN.EXE` (829,952 bytes) | `49d57871b57278270af929dcdf6e3f7ccf3b1adc` |
| `GAMEDATA\BIN\BINKW32.DLL` | `8dc84b70f15c3de093bbeddb5f0ea58fdca9090b` |
| `GAMEDATA\BIN\IMUSE.DLL` | `589f25d77fa2bf5ed15d92def65692e0e9931a40` |

```sh
pip install capstone
python tools/lifter/lift_all.py <path to GAMEDATA> source/gen
make            # in the devkitPro MSYS2 shell
```

Then copy `tpm3ds.3dsx` to `SD:\3ds\tpm\` and the seven folders inside `GAMEDATA` to `SD:\3ds\tpm\data\`
(not `INSTALL`, `TPM.EXE`, `AUTORUN.INF`). Optional CIA: `sh tools/cia/build_cia.sh` (needs `makerom`).

## Troubleshooting

| Problem | Fix |
|---|---|
| "devkitPro ... is not installed" / "Python 3 is not installed" | Install them (links above), then double-click `setup.bat` again. |
| "not the version this port was made for" | Only the English retail CD is supported. |
| Black screen / back to the launcher | Check that `3ds/tpm/data/BIN/WMAIN.EXE` exists on the card; read the end of `3ds/tpm/log.txt`. |
| First start seems stuck | The unpacking takes several minutes; wait until the percentage reaches 100. |
