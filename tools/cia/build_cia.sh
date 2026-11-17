#!/bin/sh
# Builds tpm3ds.cia from tpm3ds.elf + tpm3ds.smdh (run after make).
# assets/banner.bnr (banner picture + jingle, made by tools/cia/make_banner.sh) is added when it exists.
cd "$(dirname "$0")/../.." || exit 1
BANNER=""
[ -f assets/banner.bnr ] && BANNER="-banner assets/banner.bnr"
/c/devkitPro/tools/bin/makerom.exe -f cia -o tpm3ds.cia -elf tpm3ds.elf -rsf tools/cia/tpm3ds.rsf     -icon tpm3ds.smdh -exefslogo -target t -ignoresign -ver 1 $BANNER
