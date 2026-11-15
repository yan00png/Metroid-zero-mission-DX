# Port do mzm para PS2 (jogo completo + renderizador gsKit).
# Passos:  ./prepare_mzm.sh /caminho/mzm /caminho/rom_us.gba   ->  TARGET=ps2 ./build_game.sh   ->  make
# (precisa de PS2DEV/PS2SDK/GSKIT no ambiente)
EE_BIN = mzm_game.elf
GAME_OBJS = $(wildcard build/obj-ps2/*.o)
EE_CFLAGS += $(XFLAGS) -MMD   # -MMD: dependencias de header (mudar uma struct em .h recompila quem a usa)
EE_OBJS = src/gba_mem.o src/gfx_cache.o src/render.o src/menu.o src/platform/crash.o src/backend_gs.o \
          src/platform/platform.o src/platform/bios.o src/platform/dma.o src/platform/stubs.o \
          src/platform/input_ps2.o src/nes/nes.o src/nes_run.o src/platform/game_main.o src/platform/storage.o src/platform/rom.o src/platform/save.o src/platform/audio_ps2.o $(patsubst %.c,%.o,$(wildcard src/platform/irx/*.c)) $(GAME_OBJS)

EE_INCS += -Iinclude -Isrc -Isrc/platform -I$(GSKIT)/include
EE_CFLAGS += -std=gnu11 -O1 -w -fpermissive
src/nes/nes.o: EE_CFLAGS += -O3
EE_LDFLAGS += -L$(GSKIT)/lib $(shell cat platform_defsyms.txt build/mzm/ewram_defsyms.txt | grep -v '^\#' | sed 's/^/-Wl,--defsym=/')
EE_LIBS += -laudsrv -lfileXio -lmc -lpatches -lgskit -ldmakit -lpad -ldebug -leedebug -lm

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) src/*.o src/platform/*.o src/*.d src/platform/*.d

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

-include $(wildcard src/*.d src/platform/*.d)
