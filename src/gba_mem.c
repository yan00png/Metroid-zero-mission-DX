/* Memoria emulada do GBA (so o que a PPU precisa). */
unsigned char gGbaEwram[256 * 0x400];
unsigned char gGbaIwram[32 * 0x400];
unsigned char gGbaIo[0x400] __attribute__((aligned(4)));
unsigned char gGbaPalram[0x400] __attribute__((aligned(4)));
unsigned char gGbaVram[96 * 0x400] __attribute__((aligned(4)));
unsigned char gGbaOam[0x400] __attribute__((aligned(4)));
unsigned char gGbaSram[32 * 0x400];
unsigned char gGbaRom[0x1000]; /* placeholder: mapeamento real na fase de dados */
