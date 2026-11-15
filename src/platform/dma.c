/* DMA do GBA: copia/preenchimento imediato (sem hardware de DMA). */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint32_t u32;

void DmaTransfer(u8 channel, const void *src, void *dst, u32 len, u8 bitSize)
{
    (void)channel; (void)bitSize;
    memmove(dst, src, len);
}

void BitFill(u8 channel, u32 value, void *dst, u32 len, u8 bitSize)
{
    (void)channel;
    /* GBA: o DMA ignora os bits baixos do endereco; no EE um sw/sh desalinhado daria Address Error */
    if (bitSize == 32) { u32 *d = (u32 *)((uintptr_t)dst & ~(uintptr_t)3); for (u32 i = 0; i < len / 4; i++) d[i] = value; }
    else { uint16_t *d = (uint16_t *)((uintptr_t)dst & ~(uintptr_t)1); for (u32 i = 0; i < len / 2; i++) d[i] = (uint16_t)value; }
}

/* Executa o que o jogo programou nos registradores de DMA (so transferencias imediatas). */
/* DMA de HBlank (haze, gradiente): o jogo grava uma tabela de 160 linhas x "count" halfwords e o GBA copia uma linha da tabela
 * para os registradores no fim de cada scanline. Aqui so guardamos a configuracao; o renderizador consulta o valor de cada
 * linha com PlatformHblankValue(). Canais de som (inicio especial, 0x3000) ficam de fora. */
extern unsigned char gGbaIo[];
static struct { int on; const uint16_t *src; uintptr_t dst; int count; } sHb[4];

int PlatformHblankValue(uintptr_t addr, int y, uint16_t *out)
{
    for (int c = 0; c < 4; c++)
        if (sHb[c].on && addr >= sHb[c].dst && addr < sHb[c].dst + (uintptr_t)sHb[c].count * 2 && !(addr & 1))
        {
            *out = sHb[c].src[y * sHb[c].count + (addr - sHb[c].dst) / 2];
            return 1;
        }
    return 0;
}

void PlatformDmaTrigger(volatile unsigned int *r)
{
    u32 ctrl = r[2] >> 16, count = r[2] & 0xFFFF;
    int ch = (int)(((intptr_t)r - (intptr_t)(gGbaIo + 0xB0)) / 12);
    if (ch >= 0 && ch < 4)
    {
        if (!(ctrl & 0x8000)) sHb[ch].on = 0;
        else if ((ctrl & 0x3000) == 0x2000 && !((ctrl >> 10) & 1))   /* HBlank, 16 bits */
        {
            sHb[ch].on = 1; sHb[ch].src = (const uint16_t *)(uintptr_t)(r[0] & ~1u);
            sHb[ch].dst = (uintptr_t)(r[1] & ~1u); sHb[ch].count = count ? (int)count : 0x10000;
            return;                               /* repetido: continua habilitado */
        }
    }
    if (!(ctrl & 0x8000)) return;                 /* nao habilitado */
    r[2] &= 0x7FFFFFFF;                           /* "terminou": bit de enable cai */
    if (ctrl & 0x3000) {  return; }                    /* inicio em VBlank/HBlank/especial: nao suportado ainda */
    int is32 = (ctrl >> 10) & 1, sctl = (ctrl >> 7) & 3, dctl = (ctrl >> 5) & 3;
    if (!count) count = 0x10000;
    /* a GBA ignora os bits baixos dos enderecos do DMA; o EE real faria Address Error num lw/lh desalinhado */
    uintptr_t amask = is32 ? ~(uintptr_t)3 : ~(uintptr_t)1;
    uint8_t *src = (uint8_t *)((uintptr_t)r[0] & amask), *dst = (uint8_t *)((uintptr_t)r[1] & amask);
    int unit = is32 ? 4 : 2;
    int sstep = sctl == 0 ? unit : sctl == 1 ? -unit : 0, dstep = dctl == 1 ? -unit : dctl == 2 ? 0 : unit;
    while (count--)
    {
        if (is32) *(u32 *)dst = *(u32 *)src; else *(uint16_t *)dst = *(uint16_t *)src;
        src += sstep; dst += dstep;
    }
}
