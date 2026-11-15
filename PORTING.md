# Port nativo de Metroid: Zero Mission para PS2 — como foi feito

Este documento descreve o processo de portagem do decomp **metroidret/mzm** (Metroid: Zero Mission, ROM US Rev.00) para o
PlayStation 2. O port é **nativo**: o código do jogo, escrito em C no decomp, é compilado para o EE (MIPS R5900) e roda
direto. Não há emulação de CPU (ARM7TDMI) nem de PPU. O que o GBA fazia em hardware (vídeo, DMA, BIOS, som) foi
substituído por uma camada de plataforma e por um renderizador que desenha no GS.

Commit base do decomp: `7708d105cb3537f941449a9ac74795bb435493fa` (`patches/mzm_commit.txt`).

---

## 1. Ideia central

| GBA | PS2 |
|---|---|
| CPU ARM7 executa o jogo | O jogo (C) é compilado com `mips64r5900el-ps2-elf-gcc` e roda no EE |
| PPU desenha BGs/OBJs a partir de VRAM/PALRAM/OAM/IO | Esses blocos são **buffers reais na RAM** (`gGbaVram`, `gGbaPalram`, `gGbaOam`, `gGbaIo`); o renderizador lê esses buffers e desenha quads no GS |
| DMA em hardware | `PlatformDmaTrigger`: executa a transferência na hora em que o controle é gravado (e registra o que for de HBlank) |
| BIOS (LZ77, CpuSet, divisão…) | Reimplementada em C (`src/platform/bios.c`) |
| ROM no cartucho | A ROM original fica no pen-drive; o ELF **não leva assets** |
| SRAM (save) | Arquivo `save/metroid_zero.sav` ao lado do ELF |
| Som (motor em ARM/Thumb) | Motor de áudio próprio em C (a partir do asm original) + sintetizador PSG a 48 kHz, saída via audsrv |

Os endereços do GBA não são literais (`0x04000000`…): são **símbolos** (`gGbaIo`, `gGbaPalram`, …). Isso importa para o código
novo: usar o literal não funciona (já causou um bug com o haze, ver seção 8).

---

## 2. Ambiente e ferramentas

- **ps2dev / ps2sdk / gsKit** montados no sandbox (`PS2DEV=/usr/local/ps2dev`, `PS2SDK`, `GSKIT`).
- Teste rápido em **PCSX2** (headless, com screenshots) e teste final em **PS2 real** via conversor HDMI. As cores estranhas
  e o chiado nas fotos vêm do conversor, não do jogo.
- Compilação das duas variantes: `TARGET=host` (binário i386 que roda a lógica sem tela, para validar) e `TARGET=ps2`.
- O PCSX2 aceita acessos desalinhados e outros erros que o EE real transforma em exceção. Por isso existe
  `src/platform/crash.c`, que mostra EPC/BadVAddr/registradores na tela (usar `addr2line` no mesmo ELF).

---

## 3. Preparação do decomp (`prepare_mzm.sh`)

O script copia o mzm para `build/mzm` (sem tocar no original) e aplica as correções mínimas de portabilidade:

1. **`static` em definições após `extern`**: o agbcc aceita, o GCC não (`patches/static_fixups.txt`).
2. **Constantes float** (`CONVERT_SECONDS`): a conta em float do R5900 arredonda para baixo (`1.f/60*60` vira 0). Soma-se `0.001f`.
3. **BIOS**: `svc N` vira `PlatformSyscall(N)`; `CpuFastSet` passa a receber o controle completo (32 bits).
4. **SRAM**: o GBA copia a rotina de gravação para a pilha e a executa de lá. No port, as rotinas são chamadas direto.
5. **DMA**: `DMA_SET` chama `PlatformDmaTrigger`.
6. **Dados extraídos da ROM** (`tools/extractor.py`). Sem ROM, geram-se stubs só para conferir a compilação.
7. **Símbolos fixos de EWRAM** do linker script do GBA viram deslocamentos no buffer `gGbaEwram` (`ewram_defsyms.txt`).
8. **Áudio**: remove a espera de `VCOUNT` em `SetupSoundTransfer` (não há scanline).
9. **Código copiado para RAM e chamado por ponteiro** (Thumb no GBA, inútil em MIPS): clipdata, haze e a fuga de Chozodia
   passam a chamar as funções C direto.
10. **Dados contíguos na ROM e separados aqui**: o minimapa (`sMinimapTilesGfx` + `sPauseScreen_40f4c4`) vira um objeto único.

Pendente: o patch do `hud.c` (`gPortHudOamCount`, usado para ancorar o HUD no 16:9) e a cópia dos arquivos de áudio ainda
**não** estão no `prepare_mzm.sh`; hoje vivem só no `build/mzm` já preparado. Sem isso o projeto não é reproduzível do zero.

---

## 4. Build

```
./prepare_mzm.sh /caminho/mzm /caminho/mzm_us_baserom.gba   # prepara build/mzm
TARGET=ps2 ./build_game.sh                                  # objetos do jogo em build/obj-ps2
make                                                        # linka mzm_game.elf (gsKit + camada de plataforma)
```

Variante de teste no PCSX2 (lê a ROM do `host:`): `make XFLAGS=-DSTORAGE_HOST` (é preciso reconstruir os objetos de
`src/` e `src/platform/`; os do jogo podem ser reaproveitados). Flags usadas hoje: `-O1` (jogo e port), `-std=gnu11`,
`-fpermissive`, `-DREGION_US -DNON_MATCHING -DBUGFIX -DPORT_NATIVE -DPORT_RAW_AUDIO`.

---

## 5. A ROM não vai no ELF (`romsplit` + `rom.c`)

Decisão do projeto: o ELF não contém assets. O jogo lê `metroid_zero.gba` (ROM US) do pen-drive (FAT32/exFAT), na mesma
pasta do ELF.

- `tools/romsplit.py` roda sobre o assembly de cada arquivo: acha constantes de `.rodata` sem relocação cujo conteúdo é uma
  cópia exata de uma região da ROM, e as transforma em `.space` no `.bss`. Cada uma é registrada na seção `romtab`
  (endereço, offset na ROM, tamanho).
- `src/platform/rom.c` (`RomLoad`) lê os 16 MB para a RAM, confere **cabeçalho** (`ZEROMISSION`, região `E`, versão 0),
  **tamanho** e **CRC32** do arquivo inteiro, e copia cada trecho de `romtab` para o seu símbolo.
- Se a ROM estiver ausente ou incompatível, aparece um aviso na tela (pen-drive não encontrado, ROM não encontrada, outra
  região/versão, tamanho ou checksum errado, com detalhe do erro de leitura).
- Não há carregamento sob demanda: o jogo é leve e tudo fica na RAM.

---

## 6. Camada de plataforma (`src/platform/`)

- `platform.c`: ciclo de frame (`Halt`/`VBlankIntrWait` = handler de VBlank do jogo + desenho no GS + leitura do controle),
  atalho do menu (Start + Select) e perfilamento por ciclos de CPU (294,912 MHz).
- `bios.c`: LZ77, CpuSet/CpuFastSet e demais funções do BIOS em C.
- `dma.c`: DMA imediato e **registro de DMA de HBlank** (config por canal, consultada por scanline pelo renderizador).
- `stubs.c`: partes sem implementação (cabo de link, MultiBoot).
- `storage.c` + `irx/`: USB (usbd, usbmass_bd, bdm, fileXio…); montagem do pen-drive e acesso a arquivos.
- `save.c`: SRAM em `save/metroid_zero.sav`.
- `input_ps2.c`: libpad, remapeamento de botões (arquivo `save/controls.cfg`) e lançamento de outros programas
  (OPL, uLaunchELF, browser) com reset da IOP.
- `audio_ps2.c` + `audio_port/`: áudio (ver seção 9).
- `crash.c`: relatório de exceção/travamento.

---

## 7. Renderizador (gsKit)

Arquivos: `src/gfx_cache.c`, `src/render.c`, `src/backend_gs.c`, `src/gfx_backend.h`.

**Cache (`gfx_cache.c`)** — traduz VRAM/paleta para formatos do GS e só refaz o que mudou:
- Camadas de BG pré-compostas em texturas **T8** (4bpp vira `banco*16 + cor`; índice 0 = transparente).
- Atlas de OBJ em T4/T8 (256×256) e CLUTs por banco.
- Comparação da VRAM com uma cópia de sombra por unidade de 32 bytes; marca tiles como vazios para o desenho pular áreas
  transparentes.
- BGs afins (modos 1–2) com construção própria (charBase/scrBase).

**Desenho (`render.c`)**:
- Ordem por prioridade, BG com wrap, scroll por linha (HBlank), sprites (flip, 1D/2D, 4/8bpp), alpha de OBJ e de camada,
  fade por BLDY.
- **Efeitos de HBlank** (calor, ondulação, degradê, haze): o callback de HBlank e as DMAs registradas são aplicados por
  scanline.
- **Janelas WIN0/WIN1**: partição em retângulos com máscara de camadas, recorte dos quads e UVs.
- **Janela de OBJ** (usada na tela de status da pausa, na silhueta da Samus): máscara por pixel montada na CPU a partir dos
  sprites em modo janela, convertida em células por linha.
- **BG afim**: recorte do polígono + `BeQuadAffine`. **OBJ afim**: texturas de rascunho 64×64 (máx. 32 por frame).
- **Widescreen 16:9 nativo** (`WS_PAD = 22` colunas de cada lado), com o HUD ancorado nos cantos.
- **Mosaico**: não implementado de propósito, porque o jogo nunca o usa (a flag `SPRITE_STATUS_MOSAIC` só escolhe o slot de
  matriz afim e o `REG_MOSAIC` só recebe 0).

**Backend (`backend_gs.c`)** — gsKit: quads texturizados, CLUT por banco, faixas pretas fora do quadro do jogo, modos de vídeo
(160p, 480i, 480p, 1080i), tamanhos (2.0x / 2.5x / FIT), aspectos (16:9 / 4:3 / 3:2), filtro bilinear (com
`REGION_CLAMP` por quad para não vazar texels vizinhos) e multipassada para 480p/1080i (estilo Super Metroid PS2/OPL).

---

## 8. Problemas encontrados e correções

- **Acessos desalinhados** funcionavam no PCSX2 e travavam no PS2 real (fix15). Resultado: gameplay e pausa passaram a
  funcionar no hardware.
- **Haze/HBlank sem efeito**: o código usava `0x04000000` literal em vez de `gGbaIo`/`gGbaPalram`. Corrigido.
- **Menu "Exit Game" abria a tela do memory card**: faltava resetar a IOP e carregar módulos de memory card da ROM antes de
  `LoadExecPS2`. Corrigido (confirmação no hardware pendente).
- **Bilinear com retângulos sólidos em sprites espelhados**: em `ClampRegion`, UVs espelhados chegam como `borda − 1/16`;
  `floorf` caía no texel anterior (−1 = 1023 em 10 bits), invertendo a região de clamp. Corrigido com tolerância de 1/8 de
  texel e limites 0..1023 (confirmado no PS2 real).
- **Menus**: sobreposição de rótulos/valores e texto cortado nas páginas Control e Perf; corrigido com rótulos curtos,
  painéis maiores e reposicionamento.

---

## 9. Áudio

O motor de áudio original é ARM/Thumb (`asm/audio_internal.s`, `asm/soundcode.s`). O port usa um motor **próprio em C**, escrito a
partir desse assembly (não usa mais o motor do NX/Switch nem o sintetizador do 3DS):

- `audio_port/ps2_engine.c`: faixas, instrumentos, mixer de amostras de 8 bits (SoundCodeA/B/C) e ponte dos registradores PSG.
  Segue o original, inclusive peculiaridades (arredondamento do reverb, corte de 9 bits, mistura de bits em `unk_4eb4`).
  As FIFOs A (→ direita) e B (→ esquerda) são entregues sem mistura prévia; o roteamento L/R vem de `SOUNDCNT_H`.
- `audio_port/ps2_psg.c` + `include/ps2_psg.h`: 2 quadrados, onda e ruído gerados **a 48 kHz** a partir dos registradores (gIoMem).
- `src/platform/audio_ps2.c`: reamostragem cúbica (Catmull-Rom) para 48 kHz, soma DirectSound (x2 / x1 conforme 100%/50%) + PSG,
  limitador suave e controle de taxa (±2%) para manter ~1 quadro no buffer do audsrv.
- `audio.c`, `audio_wrappers.c`, `music_wrappers.c` em `audio_port/` são o código do decomp com adaptações do port;
  `prepare_mzm.sh` (passo 11) copia tudo para `src/`.

Licença: o motor novo foi escrito a partir do assembly original e da documentação do hardware, mas o autor (Claude) já tinha
lido o motor NX antes; para distribuição pública convém uma revisão humana. Os dados/código do decomp são da Nintendo.

---

## 10. Menu do frontend (Start + Select)

Abre por cima do jogo (o jogo fica parado). Páginas:
- **Resume game**.
- **Video**: modo, tamanho (480i), aspecto, filtro, barras de desempenho, monitor de FPS.
- **Controls**: remapeamento de A, B, L, R, Start e Select (persistente), com "reset default".
- **Debug GS**: liga/desliga BG, OBJ, afins, overlay, uploads; PERF (ms por parte) e PERF TEXT.
- **Exit game**: submenu com OPL, uLaunchELF e Browser.

O **PERF** mostra, em ms: GAME LOGIC, SYSTEM, LAYER CACHE, TEXTURE UPLOAD, DRAW LIST, GS + VSYNC, CPU BUSY e FRAME/FPS.

---

## 11. Desempenho (estado)

Medição em 480p, bilinear, 16:9 nativo (PS2 real): ~53–54 fps, frame ≈ 18,4–18,9 ms. Dividido em LOGIC ≈ 5,6–6,2 ms,
CACHE ≈ 2,5, UPLD ≈ 1,3, DRAW ≈ 1,1 e GS ≈ 7,1–7,3. No 480p o jogo espera o GS terminar antes de seguir, então os tempos
de CPU e GS se somam e estouram o orçamento de 16,7 ms por ~1,5 ms.

Otimizações já aplicadas: reemissão de CLAMP só quando muda, prefetch na comparação da VRAM e pulo de tiles transparentes
nas camadas de BG (mais efetivo no 16:9). Ideias pendentes: pular camadas cobertas por camadas opacas, não pintar o backdrop
onde nada o mostra, dividir a linha CACHE no PERF, revisar flags de compilação (`-O1` hoje), upload parcial de camadas de BG.

---

## 12. Pendências

- Confirmar no hardware: submenu Exit (mzm_exit2), janelas, perfil de desempenho das últimas otimizações.
- Núcleo de NES para o Metroid original (ROM já extraída e mapper iNES/MMC1 identificado; falta o núcleo).
- Salvar as configurações de vídeo entre sessões.
- Reprodutibilidade: patch do `hud.c` e arquivos de áudio no `prepare_mzm.sh`.
- 42 arquivos de dados ainda são stubs; precisa levantar quais aparecem em cena.
- Revisão humana da licença do áudio (motor novo escrito a partir do asm original; ver seção 9).
- Carregamento da ROM mais rápido (CRC32 por slicing-by-8 e/ou cache da verificação).
