#!/usr/bin/env python3
"""Tira dados constantes que sao copias exatas de regioes da ROM do ELF.
Uso: romsplit.py in.s out.s rom.gba tag [min_bytes]
 - monta in.s, acha simbolos de .rdata sem relocacao cujo conteudo existe na ROM
 - reescreve o assembly: esses simbolos viram .space em .bss (sem bytes no ELF)
 - registra (endereco, offset_rom, tamanho) na secao 'romtab' para o carregador em tempo de execucao."""
import sys, re, subprocess, os, tempfile, struct
inp, outp, romp, tag = sys.argv[1:5]
MINB = int(sys.argv[5]) if len(sys.argv) > 5 else 32
P = "mips64r5900el-ps2-elf-"
s = open(inp).read().split("\n")
tmp = tempfile.mkdtemp()
o1 = os.path.join(tmp, "a.o")
subprocess.check_call([P + "gcc", "-c", "-x", "assembler", "-o", o1, inp])
# secao .rodata (indice) e simbolos que pertencem A ELA (simbolos de .data/.bss nao podem ser confundidos com offsets de .rodata)
rd = subprocess.check_output([P + "readelf", "-S", "-W", o1], text=True)
idx = None
for line in rd.split("\n"):
    m = re.match(r"\s*\[\s*(\d+)\]\s+\.rodata\s", line)
    if m: idx = int(m.group(1))
if idx is None:
    open(outp, "w").write("\n".join(s)); sys.exit(0)
bin1 = os.path.join(tmp, "rd.bin")
subprocess.check_call([P + "objcopy", "-O", "binary", "--only-section=.rodata", o1, bin1])
data = open(bin1, "rb").read()
syms = []
for line in subprocess.check_output([P + "readelf", "-s", "-W", o1], text=True).split("\n"):
    p = line.split()
    # Num: Value Size Type Bind Vis Ndx Name
    if len(p) == 8 and p[3] == "OBJECT" and p[6] == str(idx):
        syms.append((p[7], int(p[1], 16), int(p[2], 0)))
# relocacoes em .rdata
rel = subprocess.check_output([P + "readelf", "-r", "-W", o1], text=True)
relocs = []
cur = None
for line in rel.split("\n"):
    m = re.match(r"Relocation section '([^']+)'", line)
    if m: cur = m.group(1); continue
    if cur and cur.startswith(".rel.rodata") or (cur and cur.startswith(".rela.rodata")):
        m = re.match(r"\s*([0-9a-f]{8,16})\s", line)
        if m: relocs.append(int(m.group(1), 16))
relocs.sort()
import bisect
rom = open(romp, "rb").read()
matched = {}
for name, off, size in syms:
    if size < MINB or off + size > len(data): continue
    i = bisect.bisect_left(relocs, off)
    if i < len(relocs) and relocs[i] < off + size: continue
    blk = data[off:off + size]
    if blk.count(blk[0]) == size: continue          # preenchimento constante: sem valor
    r = rom.find(blk)
    if r >= 0: matched[name] = (r, size)
# reescreve o assembly
out = []
i = 0
datadir = re.compile(r"^\s+\.(ascii|asciz|byte|half|word|space|2byte|4byte|8byte|dword|short|long|zero|string)\b")
lastalign = 2
roml = []
while i < len(s):
    ln = s[i]
    m = re.match(r"^\s+\.align\s+(\d+)", ln)
    if m: lastalign = int(m.group(1))
    m = re.match(r"^([A-Za-z_\.\$][\w\.\$]*):\s*$", ln)
    if m and m.group(1) in matched:
        name = m.group(1); r, size = matched[name]
        out.append(f"\t.pushsection .bss\n\t.align {max(lastalign,2)}\n{name}:\n\t.space {size}\n\t.popsection")
        roml.append((name, r, size))
        i += 1
        while i < len(s) and datadir.match(s[i]): i += 1
        continue
    out.append(ln); i += 1
if roml:
    out.append("\t.pushsection romtab,\"aw\",@progbits")
    for name, r, size in roml:
        out.append(f"\t.word {name}, {r}, {size}")
    out.append("\t.popsection")
open(outp, "w").write("\n".join(out) + "\n")
sys.stderr.write(f"{tag}: {len(roml)} blocos / {sum(x[2] for x in roml)} bytes\n")
