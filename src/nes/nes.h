#ifndef NES_H
#define NES_H
#include <stdint.h>
/* Emulador de NES (so o necessario para Metroid: 6502, PPU, MMC1, APU) em C portatil. */
#define NES_W 256
#define NES_H_PX 240
#define NES_PAD_A 0x01
#define NES_PAD_B 0x02
#define NES_PAD_SELECT 0x04
#define NES_PAD_START 0x08
#define NES_PAD_UP 0x10
#define NES_PAD_DOWN 0x20
#define NES_PAD_LEFT 0x40
#define NES_PAD_RIGHT 0x80
#define NES_AUDIO_RATE 48000

extern const uint32_t kNesPalette[64];        /* 0x00BBGGRR por indice do NES */
extern uint16_t gNesPix[NES_H_PX * NES_W];    /* quadro 256x240 pronto em CT16 do GS (bit 15 ligado) */

/* Metroid fica dentro da ROM do Zero Mission (LZ proprio, ponteiro em 0x7D80CC): extrai os 128 KB de PRG. 0 = ok */
int NesExtractMetroid(const uint8_t *gba, uint32_t gbaSize, uint8_t *prg128k);
/* prg: 128 KB (8 bancos de 16 KB), MMC1 com CHR-RAM */
void NesInit(const uint8_t *prg, uint32_t size);
void NesFrame(uint8_t pad);                    /* roda um quadro (262 linhas) */
int NesAudioTake(int16_t *dst, int max);       /* amostras mono 48 kHz geradas desde a ultima chamada */
void NesStats(unsigned *badOps, unsigned *lastBad);
#endif
