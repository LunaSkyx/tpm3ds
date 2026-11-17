#!/bin/sh
# Runs the 3dsx in Azahar and closes the emulator by itself once the game logs its exit (Azahar otherwise stays open on
# a black window after the game ends). Usage: tools/azahar_test.sh [max_seconds]
A="$APPDATA/Azahar/sdmc/3ds/tpm"
cp tpm3ds.3dsx "$A/" || exit 1
rm -f "$A/log.txt"
"/c/Program Files/Azahar/azahar.exe" "$A/tpm3ds.3dsx" >/dev/null 2>&1 &
PID=$!
i=0; max=${1:-900}
while [ $i -lt $max ]; do
  sleep 2; i=$((i+2))
  grep -q "^\[ *[0-9]* ms\] exit:" "$A/log.txt" 2>/dev/null && break
done
sleep 1; taskkill //PID $PID //F >/dev/null 2>&1; taskkill //IM azahar.exe //F >/dev/null 2>&1
echo "done after ${i}s"
