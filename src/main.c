/* Demo do renderizador nativo: usa headers/macros do mzm (gba/display.h, io.h) para
 * preencher a "VRAM do GBA" e chama o renderizador (GS). Exercita: BG 4bpp e 8bpp,
 * scroll, sprites 4bpp/8bpp, sprite afim (rotacao), alpha de OBJ e fade (BLDY). */
#include "gba/display.h"
#include "io.h"
#include "gfx_cache.h"
#include "gfx_backend.h"
#include <string.h>

#define RGB(r, g, b) ((r) | ((g) << 5) | ((b) << 10))

static void Tile4(u8 *dst, const char rows[8][9], const int *map)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x += 2)
            dst[y * 4 + (x >> 1)] = map[(int)rows[y][x]] | (map[(int)rows[y][x + 1]] << 4);
}

static void SetupScene(void)
{
    u16 *pal = (u16 *)PALRAM_BASE;
    u16 *opal = (u16 *)PALRAM_OBJ;
    pal[0] = RGB(2, 2, 8);
    pal[1] = RGB(31, 31, 31);
    pal[2] = RGB(8, 10, 20);
    pal[3] = RGB(31, 20, 4);
    pal[4] = RGB(12, 4, 4);
    pal[5] = RGB(31, 4, 4);  /* BG 8bpp: faixas */
    pal[6] = RGB(31, 24, 4);
    pal[7] = RGB(4, 24, 8);
    pal[8] = RGB(4, 8, 31);
    opal[1] = RGB(31, 31, 0);
    opal[2] = RGB(31, 8, 8);
    opal[3] = RGB(0, 0, 0);
    opal[10] = RGB(4, 28, 4); /* OBJ 8bpp */
    opal[11] = RGB(31, 31, 31);

    u8 *chr = (u8 *)VRAM_BASE;
    /* BG 4bpp (charblock 0): tile 1 = xadrez, tile 2 = tijolo */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x += 2)
        {
            int a = ((x >> 2) ^ (y >> 2)) & 1 ? 1 : 2;
            int b = (((x + 1) >> 2) ^ (y >> 2)) & 1 ? 1 : 2;
            chr[1 * 32 + y * 4 + (x >> 1)] = a | (b << 4);
            int br = (y == 0 || y == 4 || x == 0 || ((y > 4) && x == 4)) ? 4 : 3;
            int br2 = (y == 0 || y == 4 || ((y > 4) && x + 1 == 4)) ? 4 : 3;
            chr[2 * 32 + y * 4 + (x >> 1)] = br | (br2 << 4);
        }
    /* BG 8bpp (charblock 1): tile 1 = faixas diagonais com indices 5..8 */
    u8 *chr8 = (u8 *)VRAM_BASE + 0x4000;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) chr8[1 * 64 + y * 8 + x] = 5 + (((x + y) >> 1) & 3);

    u16 *bg0 = (u16 *)((u8 *)VRAM_BASE + 28 * BGCNT_VRAM_TILE_SIZE);
    u16 *bg1 = (u16 *)((u8 *)VRAM_BASE + 29 * BGCNT_VRAM_TILE_SIZE);
    u16 *bg2 = (u16 *)((u8 *)VRAM_BASE + 30 * BGCNT_VRAM_TILE_SIZE);
    for (int i = 0; i < 1024; i++) bg0[i] = 1;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
        {
            bg1[y * 32 + x] = (y >= 16 && y < 19) ? 2 : 0;
            bg2[y * 32 + x] = (y < 2) ? 1 : 0;
        }

    /* OBJ: bola 8x8 (tile 1) */
    u8 *ochr = (u8 *)VRAM_OBJ;
    static const char ball[8][9] = {"..3333..", ".332233.", "33222233", "32211223",
                                    "32211223", "33222233", ".332233.", "..3333.."};
    static const int cmap[256] = {['.'] = 0, ['1'] = 1, ['2'] = 2, ['3'] = 3};
    /* a tabela acima usa o caractere como indice: converte via função auxiliar */
    int m[128] = {0};
    m['.'] = 0; m['1'] = 1; m['2'] = 2; m['3'] = 3;
    (void)cmap;
    Tile4(ochr + 1 * 32, ball, m);

    /* OBJ afim 16x16 (tiles 8..11): losango com marca no topo para ver a rotacao */
    for (int ty = 0; ty < 2; ty++)
        for (int tx = 0; tx < 2; tx++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x += 2)
                {
                    int px[2];
                    for (int k = 0; k < 2; k++)
                    {
                        int X = tx * 8 + x + k, Y = ty * 8 + y;
                        int d = (X > 7 ? X - 8 : 7 - X) + (Y > 7 ? Y - 8 : 7 - Y);
                        px[k] = d <= 7 ? (d >= 6 ? 3 : 2) : 0;
                        if (d <= 7 && Y < 4 && (X == 7 || X == 8)) px[k] = 1;
                    }
                    ochr[(8 + ty * 2 + tx) * 32 + y * 4 + (x >> 1)] = px[0] | (px[1] << 4);
                }
    /* OBJ 8bpp 8x8 (unidade 64): xadrez verde/branco */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) ochr[64 * 32 + y * 8 + x] = (((x >> 1) ^ (y >> 1)) & 1) ? 10 : 11;

    WRITE_16(REG_BG0CNT, CREATE_BGCNT(0, 28, BGCNT_LOW_PRIORITY, BGCNT_SIZE_256x256));
    WRITE_16(REG_BG1CNT, CREATE_BGCNT(0, 29, BGCNT_HIGH_MID_PRIORITY, BGCNT_SIZE_256x256));
    WRITE_16(REG_BG2CNT, CREATE_BGCNT(1, 30, BGCNT_HIGH_PRIORITY, BGCNT_SIZE_256x256) | 0x80);
    WRITE_16(REG_DISPCNT, DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D);
}

extern int __attribute__((weak)) gDemoMaxFrames; /* usado so pelo teste no host */

int main(void)
{
    GfxCacheInit();
    BeInit();
    SetupScene();

    u16 *oam = (u16 *)OAM_BASE;
    for (int i = 0; i < 128; i++) oam[i * 4] = 0x200; /* desligados */

    static const s16 sinT[8] = {0, 181, 256, 181, 0, -181, -256, -181};
    for (u32 frame = 0;; frame++)
    {
        WRITE_16(REG_BG0HOFS, frame >> 1);
        WRITE_16(REG_BG0VOFS, frame >> 2);
        WRITE_16(REG_BG1HOFS, frame * 2);
        WRITE_16(REG_BG2HOFS, -(int)(frame >> 1));

        /* fade-in no inicio (BLDY), depois so os alvos do alpha de OBJ */
        if (frame < 60)
        {
            WRITE_16(REG_BLDCNT, 0xFF);
            WRITE_16(REG_BLDY, 16 - frame * 16 / 60);
        }
        else
        {
            WRITE_16(REG_BLDCNT, 0x0F00 | 0x2000);
            WRITE_16(REG_BLDALPHA, 8 | (8 << 8));
        }

        for (int i = 0; i < 6; i++)
        {
            int x = 20 + ((frame * (i + 2) + i * 40) % 400);
            x = x > 200 ? 400 - x : x;
            int t = (frame * 3 + i * 25) & 63;
            int y = 30 + (t < 32 ? t & 31 : 31 - (t & 31)) * 2;
            oam[i * 4 + 0] = (y & 0xFF) | (i == 5 ? 0x400 : 0); /* o ultimo e semi-transparente */
            oam[i * 4 + 1] = (x & 0x1FF);
            oam[i * 4 + 2] = 1;
        }

        /* sprite afim girando: grupo 0 (os parametros ficam no attr3 das entradas 0..3, como no GBA) */
        int a = (frame >> 1) & 63;                       /* 64 passos por volta */
        int s = (int)(__builtin_sinf(a * 6.2831853f / 64) * 256);
        int c = (int)(__builtin_cosf(a * 6.2831853f / 64) * 256);
        oam[0 * 4 + 3] = c;  oam[1 * 4 + 3] = s;
        oam[2 * 4 + 3] = -s; oam[3 * 4 + 3] = c;
        oam[6 * 4 + 0] = 70 | 0x100;                     /* afim, quadrado */
        oam[6 * 4 + 1] = 100 | (0 << 9) | (1 << 14);     /* 16x16, grupo 0 */
        oam[6 * 4 + 2] = 8;

        /* sprite 8bpp */
        oam[7 * 4 + 0] = 120 | 0x2000;
        oam[7 * 4 + 1] = 30;
        oam[7 * 4 + 2] = 64;
        (void)sinT;

        RenderFrame();
    }
    return 0;
}
