/* Traduz os dados de VRAM/paleta do jogo para formatos que o GS consome direto.
 * Independente de plataforma (testavel no host). Nao ha emulacao de scanline. */
#include <string.h>
#include "gfx_cache.h"

extern unsigned char gGbaIo[], gGbaPalram[], gGbaVram[], gGbaOam[];

BgLayerCache gBgLayer[4];
int gPalDirty, gObjDirty;
uint16_t gClutBg4[256] __attribute__((aligned(64)));
uint16_t gClutBg8[256] __attribute__((aligned(64)));
uint16_t gClutObj8[256] __attribute__((aligned(64)));
uint16_t gClutObj4[16][16] __attribute__((aligned(64)));
uint8_t gObjAtlas4[256 * 128] __attribute__((aligned(128)));
uint8_t gObjAtlas8[256 * 128] __attribute__((aligned(128)));

static uint8_t sPix[4][LAYER_STRIDE * LAYER_STRIDE] __attribute__((aligned(128)));
static uint8_t sVramShadow[0x18000];
static uint8_t sBlkDirty[96];
static uint8_t sUnitDirty[96 * 32]; /* 1 por 32 bytes de VRAM */
static uint8_t sPalShadow[0x400];

static uint16_t Io16(int off) { return *(uint16_t *)(gGbaIo + off); }

void GfxCacheInit(void)
{
    memset(sVramShadow, 0xA5, sizeof sVramShadow);
    memset(sPalShadow, 0xA5, sizeof sPalShadow);
    for (int i = 0; i < 4; i++) { gBgLayer[i].pix = sPix[i]; gBgLayer[i].sig = 0xFFFFFFFF; }
}

/* posicao de um indice de 256 cores no CLUT do GS (CSM1 troca os blocos 8-15 e 16-23) */
static inline int Swz(int i) { return (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1); }

static void BuildCluts(void)
{
    const uint16_t *p = (const uint16_t *)gGbaPalram;
    for (int i = 0; i < 256; i++)
    {
        uint16_t bg = (p[i] & 0x7FFF) | 0x8000, ob = (p[256 + i] & 0x7FFF) | 0x8000;
        gClutBg4[Swz(i)] = (i & 15) ? bg : 0;   /* 4bpp: indice 0 de cada banco transparente */
        gClutBg8[Swz(i)] = i ? bg : 0;
        gClutObj8[Swz(i)] = i ? ob : 0;
        gClutObj4[i >> 4][i & 15] = (i & 15) ? ob : 0;
    }
}

/* reconstroi so as fileiras do atlas cujo bloco de 1KB da VRAM de OBJ mudou (1KB = 32 tiles 4bpp = 16 tiles 8bpp = 1 fileira de tiles) */
static void RebuildObjAtlases(void)
{
    for (int r = 0; r < 32; r++)
    {
        if (!sBlkDirty[64 + r]) continue;
        const uint32_t *v = (const uint32_t *)(gGbaVram + 0x10000 + r * 1024);
        uint32_t *d4 = (uint32_t *)(gObjAtlas4 + r * 8 * 128);
        for (int t = 0; t < 32; t++)
            for (int y = 0; y < 8; y++) d4[y * 32 + t] = v[t * 8 + y];
        uint32_t *d8 = (uint32_t *)(gObjAtlas8 + r * 8 * 128);
        for (int t = 0; t < 16; t++)
            for (int y = 0; y < 8; y++) { d8[y * 32 + t * 2] = v[t * 16 + y * 2]; d8[y * 32 + t * 2 + 1] = v[t * 16 + y * 2 + 1]; }
    }
}

static void ComposeTile(BgLayerCache *L, int charStart, int tx, int ty, uint16_t e)
{
    int tile = e & 0x3FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1, bank = e >> 12;
    int sz = L->is8bpp ? 64 : 32;
    uint32_t addr = charStart + tile * sz;
    uint8_t *dstBase = L->pix + (ty * 8) * LAYER_STRIDE + tx * 8;
    uint8_t *emp = &L->empty[ty * 64 + tx];
    if (addr + sz > 0x18000) { for (int y = 0; y < 8; y++) memset(dstBase + y * LAYER_STRIDE, 0, 8); *emp = 1; return; }
    const uint8_t *src = gGbaVram + addr;
    uint8_t any = 0;
    for (int y = 0; y < 8; y++)
    {
        int sy = vf ? 7 - y : y;
        uint8_t *d = dstBase + y * LAYER_STRIDE;
        if (L->is8bpp)
            for (int x = 0; x < 8; x++) { d[x] = src[sy * 8 + (hf ? 7 - x : x)]; any |= d[x]; }
        else
            for (int x = 0; x < 8; x++)
            {
                int px = hf ? 7 - x : x;
                uint8_t b = src[sy * 4 + (px >> 1)];
                int c = (px & 1) ? (b >> 4) : (b & 15);
                d[x] = c ? bank * 16 + c : 0;
                any |= d[x];
            }
    }
    *emp = !any;
}

static int AnyDirty(int startByte, int endByte)
{
    if (endByte > 0x18000) endByte = 0x18000;
    for (int b = startByte >> 10; b < ((endByte + 1023) >> 10); b++) if (sBlkDirty[b]) return 1;
    return 0;
}

void GfxCacheUpdate(void)
{
    {
        const uint64_t *v = (const uint64_t *)gGbaVram; uint64_t *sh = (uint64_t *)sVramShadow;
        for (int b = 0; b < 96; b++)
        {
            int any = 0;
            for (int u = 0; u < 32; u++)
            {
                int i = (b * 32 + u) * 4;
#ifdef USE_PREFETCH
                __builtin_prefetch(&v[i + 48], 0, 0); __builtin_prefetch(&sh[i + 48], 0, 0);   /* 384 B a frente: esconde a latencia da RAM */
#endif
                int d = (v[i] != sh[i]) | (v[i + 1] != sh[i + 1]) | (v[i + 2] != sh[i + 2]) | (v[i + 3] != sh[i + 3]);
                sUnitDirty[b * 32 + u] = d;
                if (d) { sh[i] = v[i]; sh[i + 1] = v[i + 1]; sh[i + 2] = v[i + 2]; sh[i + 3] = v[i + 3]; any = 1; }
            }
            sBlkDirty[b] = any;
        }
    }
    gPalDirty = memcmp(sPalShadow, gGbaPalram, 0x400) != 0;
    if (gPalDirty) { memcpy(sPalShadow, gGbaPalram, 0x400); BuildCluts(); }

    gObjDirty = AnyDirty(0x10000, 0x18000);
    if (gObjDirty) RebuildObjAtlases();

    uint16_t dispcnt = Io16(0);
    int mode = dispcnt & 7;
    for (int bg = 0; bg < 4; bg++)
    {
        BgLayerCache *L = &gBgLayer[bg];
        int aff = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
        L->enabled = ((dispcnt >> (8 + bg)) & 1) && (mode == 0 || (mode == 1 && bg < 3) || (mode == 2 && bg >= 2));
        if (!L->enabled) { L->sig = 0xFFFFFFFF; continue; } /* ao reativar, recompoe tudo */
        L->affine = aff;

        uint16_t cnt = Io16(0x08 + bg * 2);
        if (aff)
        {
            int charBase = (cnt >> 2) & 3, scrBase = (cnt >> 8) & 0x1F, size = cnt >> 14, n = 16 << size;   /* n tiles por lado */
            if (n > 64) { L->enabled = 0; L->sig = 0xFFFFFFFF; continue; }   /* 1024x1024 nao cabe na camada de 512x512 */
            uint32_t sig = charBase | (scrBase << 3) | (size << 8) | 0x10000;
            int full = (sig != L->sig), charStart = charBase * 0x4000, mapStart = scrBase * 0x800;
            L->sig = sig; L->is8bpp = 1; L->wPx = L->hPx = n * 8;
            int charDirty = AnyDirty(charStart, charStart + 0x4000);
            if (!full && !charDirty && !AnyDirty(mapStart, mapStart + n * n)) continue;
            const uint8_t *map = gGbaVram + mapStart;
            int changed = 0;
            for (int ty = 0; ty < n; ty++)
                for (int tx = 0; tx < n; tx++)
                {
                    uint16_t e = map[ty * n + tx];
                    if (!full && e == L->shadowMap[ty * 64 + tx] && !charDirty) continue;
                    L->shadowMap[ty * 64 + tx] = e;
                    ComposeTile(L, charStart, tx, ty, e);
                    changed = 1;
                }
            if (changed || full) L->dirty = 1;
            continue;
        }
        int charBase = (cnt >> 2) & 3, is8 = (cnt >> 7) & 1, scrBase = (cnt >> 8) & 0x1F, size = cnt >> 14;
        uint32_t sig = charBase | (is8 << 2) | (scrBase << 3) | (size << 8);
        int wT = (size & 1) ? 64 : 32, hT = (size & 2) ? 64 : 32;
        int charStart = charBase * 0x4000;
        int full = (sig != L->sig);
        L->sig = sig; L->is8bpp = is8; L->wPx = wT * 8; L->hPx = hT * 8;
        int charDirty = AnyDirty(charStart, charStart + (is8 ? 0x10000 : 0x8000));

        int nblk = size == 0 ? 1 : (size == 3 ? 4 : 2);
        int mapStart = scrBase * 0x800;
        if (!full && !charDirty && !AnyDirty(mapStart, mapStart + nblk * 0x800)) continue;

        const uint16_t *maps = (const uint16_t *)(gGbaVram + mapStart);
        int changed = 0;
        for (int ty = 0; ty < hT; ty++)
            for (int tx = 0; tx < wT; tx++)
            {
                int block = size == 0 ? 0 : size == 1 ? (tx >> 5) : size == 2 ? (ty >> 5) : (((ty >> 5) << 1) | (tx >> 5));
                uint16_t e = maps[block * 1024 + (ty & 31) * 32 + (tx & 31)];
                if (!full && e == L->shadowMap[ty * 64 + tx])
                {
                    if (!charDirty) continue;
                    uint32_t u = (charStart + (e & 0x3FF) * (is8 ? 64 : 32)) >> 5;
                    if (u >= 96 * 32 || !(sUnitDirty[u] | (is8 && u + 1 < 96 * 32 && sUnitDirty[u + 1]))) continue;
                }
                L->shadowMap[ty * 64 + tx] = e;
                ComposeTile(L, charStart, tx, ty, e);
                changed = 1;
            }
        if (changed || full) L->dirty = 1;
    }
}

int GfxAssembleObj(int is8, int oneD, int base, int wT, int hT, uint8_t *out)
{
    int step = is8 ? 2 : 1, rowBytes = wT * (is8 ? 8 : 4), tileBytes = is8 ? 8 : 4;
    for (int ty = 0; ty < hT; ty++)
        for (int tx = 0; tx < wT; tx++)
        {
            int unit = (oneD ? base + (ty * wT + tx) * step : base + ty * 32 + tx * step) & 0x3FF;
            const uint8_t *src = gGbaVram + 0x10000 + unit * 32;
            if (unit * 32 + (is8 ? 64 : 32) > 0x8000) src = gGbaVram + 0x10000; /* fora do OBJ VRAM */
            for (int y = 0; y < 8; y++)
                memcpy(out + (ty * 8 + y) * rowBytes + tx * tileBytes, src + y * tileBytes, tileBytes);
        }
    return rowBytes * hT * 8;
}
