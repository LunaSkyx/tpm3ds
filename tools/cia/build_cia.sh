#!/bin/sh
# Builds tpm3ds.cia from tpm3ds.elf + tpm3ds.smdh (run after make; setup.ps1 does this for you).
# assets/banner.bnr (banner picture + jingle, made by tools/cia/make_banner.sh) is added when it exists.
cd "$(dirname "$0")/../.." || exit 1
BANNER=""
[ -f assets/banner.bnr ] && BANNER="-banner assets/banner.bnr"
# makerom: tools/cia/bin/ (downloaded by setup.ps1), else the devkitPro tools folder, else PATH
MAKEROM=makerom
for m in tools/cia/bin/makerom.exe "${DEVKITPRO:-/c/devkitPro}/tools/bin/makerom.exe" /c/devkitPro/tools/bin/makerom.exe; do
  [ -x "$m" ] && { MAKEROM="$m"; break; }
done
"$MAKEROM" -f cia -o tpm3ds.cia -elf tpm3ds.elf -rsf tools/cia/tpm3ds.rsf -icon tpm3ds.smdh -exefslogo -target t -ignoresign -ver 1 $BANNER
