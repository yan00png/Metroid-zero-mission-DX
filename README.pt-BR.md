[English](README.md) | **Português**

# Metroid: Zero Mission DX

Port **nativo** (sem emulação de CPU/PPU) de Metroid: Zero Mission para PS2, feito a partir da decompilação [metroidret/mzm](https://github.com/metroidret/mzm) e renderizado direto no GS com gsKit.

Este repositório **não contém a ROM** nem dados extraídos dela: é preciso ter a sua própria cópia legal do jogo (versão americana, Rev.00).

## Estrutura
`mzm/` e uma copia do decomp [metroidret/mzm](https://github.com/metroidret/mzm) (commit em `patches/mzm_commit.txt`), so codigo: sem ROM e sem
dados extraidos dela (gráficos, salas, etc. sao extraidos da SUA ROM na hora de compilar). As alteracoes de portabilidade ficam em
`patches/` e `audio_port/` e nao sao commitadas em `mzm/`: `prepare_mzm.sh` copia o decomp para `build/mzm` e aplica tudo la.

## Features
- Menu com várias configurações (Start + Select)
- Suporte a 16:9 nativo, 4:3, 3:2 e 16:9 esticado
- Suporte a FAT32 e exFAT
- Suporte a 1080i e 480p nativos; o padrão é 480i
- Possibilidade de personalizar os botões do controle
- Suporte a ISO em CD/DVD
- Suporte a filtros Scanline e LCD

## Como usar o ELF
Para usar o ELF você precisa da sua cópia legal da ROM do jogo (versão americana, Rev.00).

1. Renomeie a ROM para **`metroid_zero.gba`**.
2. Coloque o `metroid_zero.gba` na mesma pasta do ELF. O ELF pode ficar em qualquer lugar (pen-drive, HDD...), desde que o `metroid_zero.gba` esteja ao lado dele.
3. Abra o ELF pela aba **Apps** do OPL.

Também funciona em **ISO**: a ROM vai dentro da ISO (veja `tools/mkiso.sh`, que a monta com o ELF e a sua ROM) e o save vai para o memory card.

## Compilar
Requer ps2dev/ps2sdk/gsKit (`PS2DEV`, `PS2SDK`, `GSKIT` no ambiente), python3.
```
./prepare_mzm.sh mzm /caminho/mzm_us_baserom.gba
TARGET=ps2 ./build_game.sh && make      # gera mzm_game.elf
sh tools/mkiso.sh mzm_game.elf /caminho/mzm_us_baserom.gba metroid_zero.iso   # opcional
```
Execucao: ELF + `metroid_zero.gba` na mesma pasta (USB/HDD/CD); save em `save/` (ou memory card).

## Créditos
Este port só existe graças à **equipe da decompilação de Metroid: Zero Mission** ([metroidret/mzm](https://github.com/metroidret/mzm)), que decompilou o jogo inteiro, e a todos os [colaboradores do projeto](https://github.com/metroidret/mzm/graphs/contributors). Obrigado!

Metroid: Zero Mission é propriedade da Nintendo.

## Licenças
O decomp em `mzm/` e MIT (veja `mzm/LICENSE`). Os IRX em `src/platform/irx/` vem do ps2sdk (AFL 3.0).
O codigo do port (este repositorio) e MIT, veja `LICENSE`. Metroid: Zero Mission e propriedade da Nintendo; o projeto nao inclui ROM nem dados extraidos dela.

---

# mzm -> PS2 (fase 1: renderizador nativo no GS)

Backend de video para um port do decomp do Metroid: Zero Mission (metroidret/mzm). Sem emulacao de CPU
ou de scanline: o GS desenha direto, a partir dos dados que o jogo escreve.

- `include/gba/memory.h`: substitui o do mzm; REG_*/VRAM/PALRAM/OAM apontam para buffers no host, entao
  `WRITE_16(REG_DISPCNT, ...)` e headers (`gba/display.h`, `io.h`) funcionam sem alteracao.
- `src/gfx_cache.c`: traduz VRAM/paleta para formatos do GS (so recompoe o que mudou):
  camadas de BG pre-compostas em texturas T8 (4bpp vira banco*16+cor), atlas de OBJ em T4/T8, CLUTs.
- `src/render.c`: decide o que desenhar: ordem por prioridade, BG com wrap, sprites (flip, 1D/2D, 4/8bpp),
  sprites afins (rotacao/escala), alpha de OBJ e de camada, fade por BLDY.
- `src/backend_gs.c`: gsKit; quads texturizados, CLUT por banco, faixas pretas fora dos 240x160.
- `test/be_host.c`: backend de teste no host (amostra as mesmas texturas) para validar a logica sem GS.
- `src/main.c`: cena de demo.

Build: `make MZM=/caminho/do/mzm` (PS2DEV/PS2SDK/GSKIT no ambiente; ver setup_ps2dev.sh).

Nao suportado ainda: janelas (WIN0/WIN1/OBJ), mosaico, BGs afins (modos 1-2), modos 3-5, brilho por camada
(so fade da tela inteira), blend alpha exato por pixel (aproximado por camada), efeitos por scanline (HDMA).

## Como compilar o jogo completo

1. `./prepare_mzm.sh /caminho/mzm /caminho/mzm_us_baserom.gba` (ROM US Rev.00; extrai os dados e aplica as correcoes de `patches/`)
2. Teste no host (sem PS2): `DUMP=60,600 STOP=600 test/run_host.sh` -> quadros em `build/out/*.ppm`;
   entrada de teste: `KEYS="3@100-110"` (bit do botao GBA @ quadros).
3. PS2: `TARGET=ps2 ./build_game.sh && make` -> `mzm_game.elf` (a demo antiga do renderizador ficou em `Makefile.demo`).

Camada de plataforma em `src/platform/`: BIOS em C (LZ77, CpuSet...), DMA imediato, ciclo de frame
(Halt/VBlank = handler de VBlank do jogo + desenho no GS + leitura do controle), controle via libpad.

Estado: a abertura do jogo roda inteira no host (texto, arte da Samus, nave, nebulosa). Ainda sem audio (mudo),
sem DMA de HBlank (efeitos de calor/ondulacao), sem janelas/mosaico/BGs afins no renderizador, sem link cable.

## Menu do frontend (Start + Select)
Abre por cima do jogo (o jogo fica parado). Cima/Baixo move, X confirma, O volta.
- **Resume game**: volta ao jogo.
- **Video**: *Screen size* (2.0x / 2.5x / FIT = maior tamanho inteiro que cabe) e *Perf bars* (barras de desempenho, desligadas por padrao).
- **Exit game**: sai do jogo.
Codigo: `src/menu.c` (menu + fonte 5x7 propria), `PlatformPollInput` em `src/platform/platform.c` (deteccao do atalho; o Start chega ao jogo com 3 quadros de atraso para nao abrir a pausa antes do menu).
