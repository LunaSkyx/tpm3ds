#!/bin/sh
# assets/banner.png + assets/jingle.wav -> assets/banner.bnr, with bannertool
# (https://github.com/carstene1ns/3ds-bannertool, put bannertool.exe in tools/cia/bin/ or on PATH).
cd "$(dirname "$0")/../.." || exit 1
python -I tools/cia/make_assets.py || exit 1
BT=tools/cia/bin/bannertool.exe
[ -x "$BT" ] || BT=bannertool
"$BT" makebanner -i assets/banner.png -a assets/jingle.wav -o assets/banner.bnr
