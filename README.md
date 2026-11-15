**English** | [Português](README.pt-BR.md)

# Metroid: Zero Mission DX

A **native** port (no CPU/PPU emulation) of Metroid: Zero Mission for the PS2, built from the [metroidret/mzm](https://github.com/metroidret/mzm) decompilation and rendered straight on the GS with gsKit.

This repository **does not contain the ROM** or any data extracted from it: you need your own legal copy of the game (US version, Rev.00).

## Structure
`mzm/` is a copy of the [metroidret/mzm](https://github.com/metroidret/mzm) decomp (commit in `patches/mzm_commit.txt`), code only: no ROM and no data extracted from it (graphics, rooms, etc. are extracted from YOUR ROM at build time). The portability changes live in `patches/` and `audio_port/` and are not committed into `mzm/`: `prepare_mzm.sh` copies the decomp to `build/mzm` and applies everything there.

## Features
- Menu with several settings (Start + Select)
- Native 16:9 support, plus 4:3, 3:2 and stretched 16:9
- FAT32 and exFAT support
- Native 1080i and 480p support; the default is 480i
- Customizable controller buttons
- CD/DVD ISO support
- Scanline and LCD filters

## How to use the ELF
To use the ELF you need your own legal copy of the game ROM (US version, Rev.00).

1. Rename the ROM to **`metroid_zero.gba`**.
2. Put `metroid_zero.gba` in the same folder as the ELF. The ELF can be anywhere (USB drive, HDD...), as long as `metroid_zero.gba` is next to it.
3. Launch the ELF from the **Apps** tab of OPL.

It also works as an **ISO**: the ROM goes inside the ISO (see `tools/mkiso.sh`, which builds it from the ELF and your ROM) and the save goes to the memory card.

## Building
Requires ps2dev/ps2sdk/gsKit (`PS2DEV`, `PS2SDK`, `GSKIT` in the environment) and python3.
```
./prepare_mzm.sh mzm /path/to/mzm_us_baserom.gba
TARGET=ps2 ./build_game.sh && make      # produces mzm_game.elf
sh tools/mkiso.sh mzm_game.elf /path/to/mzm_us_baserom.gba metroid_zero.iso   # optional
```
Running: ELF + `metroid_zero.gba` in the same folder (USB/HDD/CD); the save goes to `save/` (or the memory card).

## Credits
This port only exists thanks to the **Metroid: Zero Mission decompilation team** ([metroidret/mzm](https://github.com/metroidret/mzm)), who decompiled the whole game, and to all the [project contributors](https://github.com/metroidret/mzm/graphs/contributors). Thank you!

Metroid: Zero Mission is property of Nintendo.

## Licenses
The decomp in `mzm/` is MIT (see `mzm/LICENSE`). The IRX modules in `src/platform/irx/` come from ps2sdk (AFL 3.0).
The port code (this repository) is MIT, see `LICENSE`. Metroid: Zero Mission is property of Nintendo; the project includes no ROM and no data extracted from it.

---

# mzm -> PS2 (phase 1: native renderer on the GS)

Video backend for a port of the Metroid: Zero Mission decomp (metroidret/mzm). No CPU or scanline emulation: the GS draws directly from the data the game writes.

- `include/gba/memory.h`: replaces mzm's own; REG_*/VRAM/PALRAM/OAM point to host buffers, so `WRITE_16(REG_DISPCNT, ...)` and the headers (`gba/display.h`, `io.h`) work unchanged.
- `src/gfx_cache.c`: translates VRAM/palettes into GS formats (only recomposes what changed): BG layers pre-composed into T8 textures (4bpp becomes bank*16+color), OBJ atlas in T4/T8, CLUTs.
- `src/render.c`: decides what to draw: priority order, wrapping BGs, sprites (flip, 1D/2D, 4/8bpp), affine sprites (rotation/scaling), OBJ and layer alpha, BLDY fade.
- `src/backend_gs.c`: gsKit; textured quads, per-bank CLUT, black bars outside the 240x160 area.
- `test/be_host.c`: host test backend (samples the same textures) to validate the logic without a GS.
- `src/main.c`: demo scene.

Build: `make MZM=/path/to/mzm` (PS2DEV/PS2SDK/GSKIT in the environment; see setup_ps2dev.sh).

Not supported yet: windows (WIN0/WIN1/OBJ), mosaic, affine BGs (modes 1-2), modes 3-5, per-layer brightness (whole-screen fade only), exact per-pixel alpha blending (approximated per layer), per-scanline effects (HDMA).

## How to build the full game

1. `./prepare_mzm.sh /path/to/mzm /path/to/mzm_us_baserom.gba` (US Rev.00 ROM; extracts the data and applies the fixes in `patches/`)
2. Host test (no PS2): `DUMP=60,600 STOP=600 test/run_host.sh` -> frames in `build/out/*.ppm`; test input: `KEYS="3@100-110"` (GBA button bit @ frames).
3. PS2: `TARGET=ps2 ./build_game.sh && make` -> `mzm_game.elf` (the old renderer demo is kept in `Makefile.demo`).

Platform layer in `src/platform/`: BIOS in C (LZ77, CpuSet...), immediate DMA, frame cycle (Halt/VBlank = the game's VBlank handler + drawing on the GS + reading the controller), controller through libpad.

Status: the game's intro runs entirely on the host (text, Samus art, ship, nebula). Still no audio (muted), no HBlank DMA (heat/wave effects), no windows/mosaic/affine BGs in the renderer, no link cable.

## Frontend menu (Start + Select)
Opens over the game (the game is paused). Up/Down moves, X confirms, O goes back.
- **Resume game**: back to the game.
- **Video**: *Screen size* (2.0x / 2.5x / FIT = largest integer size that fits) and *Perf bars* (performance bars, off by default).
- **Exit game**: exits the game.
Code: `src/menu.c` (menu + own 5x7 font), `PlatformPollInput` in `src/platform/platform.c` (shortcut detection; Start reaches the game 3 frames late so the pause screen does not open before the menu).
