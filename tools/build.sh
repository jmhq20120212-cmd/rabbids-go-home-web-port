#!/bin/sh
# build.sh - builds the web port from YOUR extracted copy of Rabbids Go Home (USA, RGWE41).
# Usage: tools/build.sh "/path/to/entire partition"      (the folder with sys/, files/, tmd.bin)
# Needs: python3, clang + make (Linux, macOS or WSL), the WASI SDK and Binaryen (see README.md).
set -e
P="$1"
[ -f "$P/sys/main.dol" ] && [ -f "$P/files/LynWiiRetail.elf" ] || { echo "usage: $0 \"/path/to/entire partition\" (USA disc, RGWE41)"; exit 1; }
cd "$(dirname "$0")/.."
echo "== 1/5 recompiling the game code (main.dol -> gen/)"
python3 -I tools/gen.py "$P/sys/main.dol" "$P/files/LynWiiRetail.elf" gen/
echo "== 2/5 native build"
make -j"$(nproc 2>/dev/null || echo 2)" -f Makefile.native
echo "== 3/5 running once to capture the AI module as the game loads it"
rm -f build/mem1.bin
RGH_NAND=build/nand RGH_DUMP=1 RGH_QUIET=1 timeout 600 ./build/rgh "$P" > build/dump.log 2>&1 || true
[ -f build/mem1.bin ] || { echo "the memory dump was not written: see build/dump.log"; exit 1; }
echo "== 4/5 recompiling the AI module (Ai2CppWiiFinal.rso -> gen_rso/)"
python3 -I tools/gen_rso.py "$P/files/Ai2CppWiiFinal.rso" build/mem1.bin gen_rso/ gen/syms.txt
echo "== 5/5 browser build"
BINARYEN_CORES=1 make -j"$(nproc 2>/dev/null || echo 2)" -f Makefile.wasm ${W:+W="$W"} ${BINARYEN:+BINARYEN="$BINARYEN"}   # one optimizer thread: wasm-opt needs ~6 GB of RAM
mkdir -p web/gen web/gen_rso
cp gen/syms.txt web/gen/
[ -f gen_rso/syms.txt ] && cp gen_rso/syms.txt web/gen_rso/ || true
echo "done: start the game with start-rabbids-web.bat (Windows) or: python3 serve.py --game \"$P\""
