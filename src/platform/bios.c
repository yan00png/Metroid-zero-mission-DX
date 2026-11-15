/* Funcoes do BIOS do GBA que o jogo chama, em C portavel. */
#include <stdint.h>
#include <string.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;

/* LZ77 do BIOS (tipo 0x10). 'vram' = escreve em unidades de 16 bits, como o BIOS faz. */
static void Lz77(const u8 *src, u8 *dst, int vram)
{
    u32 head = src[0] | (src[1] << 8) | (src[2] << 16) | ((u32)src[3] << 24);
    u32 size = head >> 8, out = 0;
    src += 4;
    while (out < size)
    {
        u8 flags = *src++;
        for (int i = 0; i < 8 && out < size; i++, flags <<= 1)
        {
            if (flags & 0x80)
            {
                u32 len = (src[0] >> 4) + 3, disp = ((src[0] & 15) << 8 | src[1]) + 1;
                src += 2;
                while (len-- && out < size) { dst[out] = dst[out - disp]; out++; }
            }
            else dst[out++] = *src++;
        }
    }
    (void)vram; /* o destino ja e memoria normal; a escrita por halfword nao importa aqui */
}

void LZ77UncompWram(const void *src, void *dst) { Lz77(src, dst, 0); }
void LZ77UncompVram(const void *src, void *dst) { Lz77(src, dst, 1); }
void CallLZ77UncompVram(const void *src, void *dst) { Lz77(src, dst, 1); }
void CallLZ77UncompWram(const void *src, void *dst) { Lz77(src, dst, 0); }

/* CpuSet: bits 0-20 contagem, 24 = preencher (src fixo), 26 = unidades de 32 bits */
void CpuSet(void *src, void *dst, u32 ctrl)
{
    u32 n = ctrl & 0x1FFFFF;
    if (ctrl & (1u << 26))
    {
        u32 *s = (u32 *)((uintptr_t)src & ~(uintptr_t)3), *d = (u32 *)((uintptr_t)dst & ~(uintptr_t)3);
        if (ctrl & (1u << 24)) { u32 v = *s; while (n--) *d++ = v; }
        else while (n--) *d++ = *s++;
    }
    else
    {
        u16 *s = (u16 *)((uintptr_t)src & ~(uintptr_t)1), *d = (u16 *)((uintptr_t)dst & ~(uintptr_t)1);
        if (ctrl & (1u << 24)) { u16 v = *s; while (n--) *d++ = v; }
        else while (n--) *d++ = *s++;
    }
}

/* CpuFastSet: sempre 32 bits; bit 24 = preencher */
void CpuFastSet(void *src, void *dst, u32 ctrl)
{
    u32 n = (ctrl & 0x1FFFFF), *s = (u32 *)((uintptr_t)src & ~(uintptr_t)3), *d = (u32 *)((uintptr_t)dst & ~(uintptr_t)3);
    if (ctrl & (1u << 24)) { u32 v = *s; while (n--) *d++ = v; }
    else while (n--) *d++ = *s++;
}

u16 Sqrt(u32 x)
{
    u32 r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (u16)r;
}

int Multiboot(void *mbp) { (void)mbp; return 1; }

extern void PlatformWaitVBlank(void);
void VBlankIntrWait(void) { PlatformWaitVBlank(); }
void CallVBlankIntrWait(void) { PlatformWaitVBlank(); }
int CallMultiboot(void *mbp) { return Multiboot(mbp); }
void CallSoundbias(void) {}
void TestDivarmSqrt(void) {}
