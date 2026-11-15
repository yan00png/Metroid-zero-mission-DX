#!/bin/sh
# Gera uma ISO de PS2 (CD/DVD) com o port: ELF + ROM na raiz. Uso: tools/mkiso.sh mzm.elf metroid_zero.gba saida.iso
# O ELF vira MZM.ELF e a ROM vira MZM.GBA (nomes ISO9660 8.3, como o jogo espera). Save e controles vao para o memory card.
set -e
[ $# -eq 3 ] || { echo "uso: $0 arquivo.elf rom.gba saida.iso"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp "$1" "$T/MZM.ELF"; cp "$2" "$T/MZM.GBA"
printf 'BOOT2 = cdrom0:\\MZM.ELF;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n' > "$T/SYSTEM.CNF"
xorriso -as mkisofs -iso-level 1 -V MZM_PS2 -sysid PLAYSTATION -o "$3" "$T" 2>&1 | tail -2
echo "ISO criada: $3"
