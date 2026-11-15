/* Override do include/gba/memory.h do mzm para o PS2.
 * Em vez de enderecos fixos do GBA (0x04000000...), as regioes de memoria
 * apontam para buffers no host. Assim o codigo do jogo que usa REG_*,
 * WRITE_16(), VRAM_BASE etc. continua compilando sem alteracao. */
#ifndef GBA_MEMORY_H
#define GBA_MEMORY_H

extern unsigned char gGbaEwram[];
extern unsigned char gGbaIwram[];
extern unsigned char gGbaIo[];
extern unsigned char gGbaPalram[];
extern unsigned char gGbaVram[];
extern unsigned char gGbaOam[];
extern unsigned char gGbaSram[];
extern unsigned char gGbaRom[];

#define EWRAM_BASE ((void *)gGbaEwram)
#define EWRAM_SIZE (256 * 0x400)

#define IWRAM_BASE ((void *)gGbaIwram)
#define IWRAM_SIZE (32 * 0x400)

#define REG_BASE ((void *)gGbaIo)

#define PALRAM_BASE ((void *)gGbaPalram)
#define PALRAM_OBJ ((void *)(gGbaPalram + 0x200))
#define PAL_ROW (16)
#define PAL_ROW_SIZE (PAL_ROW * 2)
#define COLORS_IN_PAL (16 * PAL_ROW)
#define PAL_SIZE (COLORS_IN_PAL * 2)
#define PALRAM_SIZE (PAL_SIZE * 2)

#define VRAM_BASE ((void *)gGbaVram)
#define VRAM_OBJ ((void *)(gGbaVram + 0x10000))
#define VRAM_SIZE (96 * 0x400)
#define VRAM_BG_SIZE (0x10000)
#define VRAM_OBJ_SIZE (0x8000)

#define OAM_BASE ((void *)gGbaOam)
#define OAM_SIZE 0x400

/* ROM e SRAM: buffers do host (a ROM real sera mapeada na fase de dados; SRAM vira arquivo no memory card). */
#define ROM_BASE ((void *)gGbaRom)
#define ROM_SIZE (32 * 0x100000)
#define SRAM_BASE ((void *)gGbaSram)
#define SRAM_SIZE (32 * 0x400)

#endif /* GBA_MEMORY_H */
