#!/usr/bin/env bash
# Compila o jogo inteiro (i386) com o renderizador e roda sem tela, salvando quadros.
# Uso: test/run_host.sh              (usa DUMP="60,120" STOP=120 KEYS="3@100-110")
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd); cd "$HERE"
EXTRA_CFLAGS="${EXTRA_CFLAGS:--fno-pie -g}" TARGET=host ./build_game.sh
S=$HERE/src
DEFS=$(cat platform_defsyms.txt build/mzm/ewram_defsyms.txt | grep -v '^#' | sed 's/^/-Wl,--defsym=/' | tr '\n' ' ')
gcc -m32 -no-pie ${EXTRA_CFLAGS:--fno-pie -g} -O1 -w -std=gnu11 -I$HERE/include -I$S -I$S/platform -o build/mzm_host \
  build/obj-host/*.o $S/gba_mem.c $S/gfx_cache.c $S/render.c $S/menu.c $S/platform/platform.c $S/platform/bios.c \
  $S/platform/dma.c $S/platform/stubs.c $S/platform/game_main.c test/be_host.c test/input_host.c test/host_stubs.c $DEFS -lm
mkdir -p build/out && cd build/out && ../mzm_host
