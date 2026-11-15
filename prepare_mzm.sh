#!/usr/bin/env bash
# Cria build/mzm: copia do mzm com as correcoes minimas de portabilidade (sem tocar no original).
# Uso: ./prepare_mzm.sh /caminho/do/mzm [mzm_us_baserom.gba]
set -euo pipefail
SRC=${1:?caminho do mzm}; ROM=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd); DST=$HERE/build/mzm
rm -rf "$DST"; mkdir -p "$HERE/build"; cp -r "$SRC" "$DST"; rm -rf "$DST/.git"
cd "$DST"
echo "mzm base: $(cat $HERE/patches/mzm_commit.txt)"

# 1) agbcc aceita 'static' na definicao apos 'extern' no header; o GCC nao. Tira o 'static' dessas linhas.
while read -r f l; do sed -i "${l}s/^static //" "$f"; done < "$HERE/patches/static_fixups.txt"

# 2) constantes float: a conta em float do R5900 arredonda para baixo (1.f/60*60 vira 0). Soma uma folga.
sed -i 's/#define CONVERT_SECONDS(s) ((s32)((s) \* FRAMES_PER_SECOND))/#define CONVERT_SECONDS(s) ((s32)((s) * FRAMES_PER_SECOND + 0.001f))/' include/types.h

# 3) BIOS: 'svc N' vira chamada de funcao; CpuFastSet recebe o controle completo (32 bits)
sed -i 's/#define SYSCALL(num) asm("svc " #num)/void PlatformSyscall(int number);\n#define SYSCALL(num) PlatformSyscall(num)/' include/syscalls.h
sed -i 's/void CpuFastSet(void \*src, void \*dst, u16 size);/void CpuFastSet(void *src, void *dst, u32 size);/' include/syscalls.h

# 4) SRAM: o GBA copia a rotina para a pilha e a executa de la (a ROM nao pode ser lida durante o acesso a SRAM).
#    No PS2 chamamos as rotinas direto.
python3 - <<'PY'
import re
p='src/sram/sram.c'; s=open(p).read()
s=re.sub(r'(void SramWriteUnchecked\(u8\* src, u8\* dest, u32 size\)\n\{).*?\n\}\n', r'\1\n    SramWriteUncheckedInternal(src, dest, size);\n}\n', s, count=1, flags=re.S)
s=re.sub(r'(u8\* SramCheck\(u8\* src, u8\* dest, u32 size\)\n\{).*?\n\}\n', r'\1\n    return SramCheckInternal(src, dest, size);\n}\n', s, count=1, flags=re.S)
open(p,'w').write(s)
PY

# 5) DMA: o jogo escreve src/dst/controle nos registradores e espera o bit de enable cair.
#    Sem hardware de DMA, executamos a transferencia na hora em que o controle e gravado.
python3 - <<'PY'
p='include/gba/dma.h'; s=open(p).read()
s=s.replace('#define DMA_SET(channel, src, dst, cnt)','void PlatformDmaTrigger(volatile unsigned int *regs);\n#define DMA_SET(channel, src, dst, cnt)',1)
s=s.replace('        dma_[2];                                                               \\','        PlatformDmaTrigger(dma_);                                              \\',1)
open(p,'w').write(s)
PY

# 6) dados extraidos da ROM (ou stubs vazios, so para conferir compilacao)
if [ -n "$ROM" ]; then
  cp "$ROM" mzm_us_baserom.gba; python3 tools/extractor.py -r us
  # o Metroid de NES embutido (desbloqueavel) tem extracao propria; fica desligado por enquanto
  mkdir -p include/extracted/data; [ -f include/extracted/data/nes_metroid.bin.inc ] || echo -n 0u > include/extracted/data/nes_metroid.bin.inc
else
  python3 - <<'PY'
import re,os,glob
for p in glob.glob('src/**/*.c',recursive=True)+glob.glob('include/**/*.h',recursive=True):
    for m in re.finditer(r'#include\s+"(extracted/[^"]+)"',open(p,errors='ignore').read()):
        t=os.path.join('include',m.group(1))
        if not os.path.exists(t):
            os.makedirs(os.path.dirname(t),exist_ok=True); open(t,'w').write('0u')
PY
  echo "AVISO: sem ROM, dados extraidos sao stubs (so compilacao)."
fi

# 7) simbolos fixos de EWRAM do linker script do GBA -> deslocamentos no buffer gGbaEwram
python3 - <<'PY'
import re
src=open('linker.ld').read()
blk=src[src.index('ewram 0x2000000'):src.index('iwram 0x3000000')]
with open('ewram_defsyms.txt','w') as f:
    for m in re.finditer(r'\.\s*=\s*(0x[0-9a-fA-F]+);\s*(\w+)\s*=\s*\.;',blk):
        f.write(f"{m.group(2)}=gGbaEwram+{m.group(1)}\n")
PY

# 8) SetupSoundTransfer espera VCOUNT mudar (varredura de tela do GBA). No port nao ha scanline: remove a espera.
sed -i '/while (READ_8(REG_VCOUNT)/d' src/audio_wrappers.c

# 9) Codigo copiado para a RAM e chamado por ponteiro (Thumb no GBA, dado inutil no MIPS).
#    - clipdata: usa a versao em C direto.
#    - haze: chama as rotinas Haze_* (C) direto; o DMA de HBlank e emulado no renderizador.
python3 - <<'PY'
import re
p='src/clipdata.c'; s=open(p).read()
s=s.replace('gClipdataCodePointer = (ClipFunc_T)(gNonGameplayRam.inGame.clipdataCode + 1);','gClipdataCodePointer = ClipdataConvertToCollision;')
s=s.replace('    DMA3_COPY_16(ClipdataConvertToCollision + 1, gNonGameplayRam.inGame.clipdataCode, sizeof(gNonGameplayRam.inGame.clipdataCode) / 2);\n','')
open(p,'w').write(s)
p='src/haze.c'; s=open(p).read()
# haze: as rotinas Haze_* ja sao C; chama direto em vez de copiar para a RAM (o DMA de HBlank e emulado em platform/dma.c)
s,_n=re.subn(r'DmaTransfer\(3, (Haze_\w+), gNonGameplayRam\.inGame\.hazeCode, [^;]*;\s*gHazeProcessCodePointer = \(HazeFunc_T\)\(gNonGameplayRam\.inGame\.hazeCode \+ 1\);', r'gHazeProcessCodePointer = (HazeFunc_T)\1;', s)
assert _n >= 7, _n
open(p,'w').write(s)
PY

# 9b) fuga de Chozodia: o callback de HBlank (escreve WIN0H por linha) era copiado para a RAM e chamado como Thumb; usa a funcao C direto
python3 - <<'PY'
p='src/chozodia_escape.c'; s=open(p).read()
s=s.replace('    DMA3_COPY_16(ChozodiaEscapeHBlank, CHOZODIA_ESCAPE_DATA.hblankCode, 0x20);\n','')
s=s.replace('CallbackSetHblank((Func_T)(CHOZODIA_ESCAPE_DATA.hblankCode + 1));','CallbackSetHblank((Func_T)ChozodiaEscapeHBlank);')
open(p,'w').write(s)
PY

# 10) Dados que o jogo le como um bloco unico: no ROM os simbolos sao vizinhos, aqui cada um e um objeto separado.
#     minimap: MinimapCopyTileGfx indexa sMinimapTilesGfx ate 0x140<<5 e o DMA copia 0x3000 bytes (= tiles + 40f4c4).
python3 - <<'PY'
p='src/data/menus/pause_screen_map_data.c'; s=open(p).read()
a=s.index('const u8 sMinimapTilesGfx[5120]'); b=s.index('const u32 sMinimapTilesAreaNamesEnglishGfx')
new='''const struct { u8 a[5120]; u32 b[1792]; } sMinimapTilesCombined = {
    {
    #include "extracted/data/menus/pause_screen/minimap_tiles.gfx.inc"
    },
    {
    #include "extracted/data/menus/pause_screen/40f4c4.gfx.inc"
    }
};
__asm__(".globl sMinimapTilesGfx\\n.set sMinimapTilesGfx, sMinimapTilesCombined\\n.globl sPauseScreen_40f4c4\\n.set sPauseScreen_40f4c4, sMinimapTilesCombined+5120\\n");
'''
s=s[:a]+new+s[b:]
open(p,'w').write(s)
PY

# 10) Patches do port: 16:9 nativo (margem de culling dos sprites, contagem de OAM do HUD)
for pf in sprite hud room nes; do patch -p1 -s < "$HERE/patches/$pf.patch"; done

# 11) Audio: audio.c/audio_wrappers.c/music_wrappers.c ja adaptados ao port + motor de som PS2 e sintetizador PSG
#     (ps2_engine.c, ps2_psg.c: escritos a partir do asm original; substituem o motor NX/GPL).
cp "$HERE"/audio_port/*.c src/
