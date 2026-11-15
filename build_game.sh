#!/usr/bin/env bash
# Compila o codigo do jogo (mzm) + camada de plataforma.
#   TARGET=host  -> binario i386 de teste que roda o jogo sem tela (valida a logica)
#   TARGET=ps2   -> objetos MIPS R5900 em build/obj-ps2 (link pelo Makefile)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); W=$HERE/build/mzm; TARGET=${TARGET:-host}
O=$HERE/build/obj-$TARGET; mkdir -p "$O"
EXCL="src/dma.c src/multiboot.c src/syscall_wrappers.c"   # substituidos em src/platform/
if [ "$TARGET" = ps2 ]; then
  export PATH=$PATH:${PS2DEV:-/usr/local/ps2dev}/ee/bin; CC=mips64r5900el-ps2-elf-gcc; ARCH=""
else CC=gcc; ARCH="-m32"; fi
CFLAGS="${EXTRA_CFLAGS:-} $ARCH -x c -std=gnu11 -O1 -fpermissive -fno-toplevel-reorder -fno-section-anchors -w -DREGION_US -DNON_MATCHING -DBUGFIX -DPORT_NATIVE -DPORT_RAW_AUDIO -I$HERE/include -I$W/include -I$W"
cd "$W"
one() { f=$1; o=$O/$(echo "$f" | tr / _).o
  [ "$o" -nt "$f" ] && return 0
  if [ "$TARGET" = ps2 ] && [ -z "${NOROMSPLIT:-}" ]; then
    t=$(mktemp -d); python3 -I tools/preproc.py "$f" charmap.txt | $CC $CFLAGS -S -o $t/a.s - &&
    python3 -I $HERE/tools/romsplit.py $t/a.s $t/b.s "$W/mzm_us_baserom.gba" "$f" 2>>$HERE/build/romsplit.log &&
    $CC -c -x assembler -o "$o" $t/b.s; r=$?; rm -rf $t; return $r
  fi
  python3 -I tools/preproc.py "$f" charmap.txt | $CC $CFLAGS -c -o "$o" - ; }
export -f one; export O CC CFLAGS W HERE TARGET NOROMSPLIT
find src -name '*.c' | sort | grep -v -x -F "$(echo $EXCL | tr ' ' '\n')" | xargs -P 8 -I{} bash -c 'one {}'
echo "objetos: $(ls $O | wc -l)"
