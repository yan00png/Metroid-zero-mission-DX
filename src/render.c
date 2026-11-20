/* Logica de desenho: decide O QUE desenhar (camadas, sprites, efeitos) e em que ordem.
 * O COMO fica no backend (GS). Sem rasterizacao por software. */
#include <string.h>
#include "gfx_cache.h"
#include "gfx_backend.h"

extern unsigned char gGbaIo[], gGbaPalram[], gGbaOam[];
static uint16_t Io16(int off) { return *(uint16_t *)(gGbaIo + off); }

static const uint8_t kW[3][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32}};
static const uint8_t kH[3][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64}};

extern int16_t gMainGameMode;
extern uint8_t gPauseScreenFlag, gCurrentCutscene;
extern int16_t gSubGameMode1;
extern uint8_t gInGameCutscene[];   /* struct InGameCutscene: [3] = numero da cena (bit 7 = ja comecou) */
int gRenderIgc;
int gRenderWsOn;   /* diagnostico (FPS MONITOR): 1 = laterais do 16:9 liberadas nesta cena */
/* 16:9 nativo: colunas extras (em pixels do GBA) desta frame. So ha mapa/jogo de verdade fora dos 240 px durante o gameplay em sala;
 * telas fixas (pausa, item, mapa, titulo, cutscenes, game over...) nao foram feitas para isso e mostrariam lixo repetido nas laterais
 * (fundos de 256 px que se repetem, sprites e VRAM antiga): la as laterais ficam pretas (BeSetWsMask). */
static int sWs;
static int WsSafeScene(void)
{
    switch (gMainGameMode)
    {
    case 4:    /* GM_INGAME */
    /* GM_CHOZODIA_ESCAPE (7) e GM_TOURIAN_ESCAPE (9) sao so cenas fixas (fuga, nave, Zebes explodindo): sem widescreen */
    case 11:   /* GM_DEMO */
        /* pausa, item, mapa baixado e dica Chozo rodam dentro do INGAME; cutscene em andamento ou jogo ainda sem sala (submodo 0) tambem nao */
        gRenderIgc = gInGameCutscene[3] & 0x7F;
        /* IGC_CLOSE_UP (4): close do rosto da Samus, tela fixa dentro do INGAME (as laterais mostrariam lixo do wrap do fundo) */
        return gPauseScreenFlag == 0 && gCurrentCutscene == 0 && gSubGameMode1 != 0 && gRenderIgc != 4;
    }
    return 0;
}
extern uint16_t gBg0XPosition, gBg1XPosition, gBg2XPosition;   /* posicao dos fundos em sub-pixels (1 px = 4) */
extern struct { uint8_t bytes[64]; } gCurrentRoomEntry;       /* so os bytes bg0Prop..bg2Prop (1..3) */
extern struct { struct { uint16_t *pDecomp; uint16_t width, height; } backgrounds[3]; } gBgPointersAndDimensions;
uint8_t gPortHudOamCount;   /* preenchido por HudUpdateOam (hud.c): slots 0..n-1 sao o HUD */
static int sEffect, sFirst, sSecond, sEva, sEvb, sEvy, sAdd, sFullFade, sDrawn, sScratch;

/* faixa [y0,y1) com scroll constante; pode cruzar a borda do mapa: ate 4 retangulos */
/* ---- janelas (WIN0/WIN1): a tela e dividida em celulas retangulares; cada uma tem a mascara de camadas visiveis
 * (bits 0-3 BG0-3, 4 OBJ, 5 efeitos de cor). Cada quad e recortado (geometria + UV) para as celulas que mostram a camada.
 * Cobre tambem janelas por linha (DMA/callback de HBlank: bomba de poder, fuga de Chozodia). Janela de OBJ (modo 2): mascara por pixel montada na CPU a partir dos sprites (tela de status) e
 * convertida em celulas por linha, so na area fora de WIN0/WIN1. */
#define MAXCELL 1600
static struct { short x0, y0, x1, y1; uint8_t mask; } sCell[MAXCELL];
static int sNCell, sWinOn;
extern void (*gHBlankCallback)(void);

static uint8_t Io8(int off) { return gGbaIo[off]; }


static uint8_t sOw[160][240];
static int BuildObjMask(uint16_t dispcnt)
{
    const uint16_t *oam = (const uint16_t *)gGbaOam;
    static uint8_t buf[64 * 64];
    int any = 0, oneD = (dispcnt >> 6) & 1;
    memset(sOw, 0, sizeof sOw);
    for (int i = 127; i >= 0; i--)
    {
        uint16_t a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        int aff = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1;
        if (((a0 >> 10) & 3) != 2 || (!aff && dbl)) continue;
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int wT = kW[shape][sz] >> 3, hT = kH[shape][sz] >> 3, is8 = (a0 >> 13) & 1;
        int oy = a0 & 0xFF, ox = a1 & 0x1FF;
        if (ox >= 384) ox -= 512;
        if (oy >= 160) oy -= 256;
        int w = wT * 8, h = hT * 8, bw = w << (aff & dbl), bh = h << (aff & dbl);
        int hf = !aff && ((a1 >> 12) & 1), vf = !aff && ((a1 >> 13) & 1);
        int rowBytes = wT * (is8 ? 8 : 4);
        if (!aff) GfxAssembleObj(is8, oneD, a2 & 0x3FF, wT, hT, buf);
        for (int py = 0; py < bh; py++)
        {
            int y = oy + py;
            if (y < 0 || y >= 160) continue;
            for (int px = 0; px < bw; px++)
            {
                int x = ox + px;
                if (x < 0 || x >= 240) continue;
                int on = 1;   /* afim: aproxima pela caixa */
                if (!aff)
                {
                    int sx = hf ? w - 1 - px : px, sy = vf ? h - 1 - py : py;
                    uint8_t b = buf[sy * rowBytes + (is8 ? sx : sx >> 1)];
                    on = (is8 ? b : (sx & 1 ? b >> 4 : b & 15)) != 0;
                }
                if (on) { sOw[y][x] = 1; any = 1; }
            }
        }
    }
    return any;
}

static void EmitCell(int x0, int x1, int y, int m)
{
    if (x0 >= x1) return;
    for (int k = sNCell - 1; k >= 0 && k >= sNCell - 24; k--)
        if (sCell[k].x0 == x0 && sCell[k].x1 == x1 && sCell[k].mask == m && sCell[k].y1 == y) { sCell[k].y1 = y + 1; return; }
    if (sNCell >= MAXCELL) return;
    sCell[sNCell].x0 = x0; sCell[sNCell].x1 = x1; sCell[sNCell].y0 = y; sCell[sNCell].y1 = y + 1; sCell[sNCell].mask = (uint8_t)m;
    sNCell++;
}

/* area [xa,xb) x [ya,yb) fora de WIN0/WIN1: o que a janela de OBJ cobre usa a mascara OBJWIN */
static void EmitOutside(int xa, int xb, int ya, int yb, int mOut, int mObj)
{
    for (int y = ya; y < yb; y++)
    {
        int cur = xa, x = xa < 0 ? 0 : xa, end = xb > 240 ? 240 : xb;
        while (x < end)
        {
            if (!sOw[y][x]) { x++; continue; }
            int rs = x; while (x < end && sOw[y][x]) x++;
            EmitCell(cur, rs, y, mOut); EmitCell(rs, x, y, mObj); cur = x;
        }
        EmitCell(cur, xb, y, mOut);
    }
}

static void BuildWindows(uint16_t dispcnt)
{
    sNCell = 0; sWinOn = 0;
    int w0 = (dispcnt >> 13) & 1, w1 = (dispcnt >> 14) & 1;
    int wo = (dispcnt >> 15) & 1;
    if (wo) wo = BuildObjMask(dispcnt);
    if (!w0 && !w1 && !wo) return;
    sWinOn = 1;
    uint8_t mk[3] = { (uint8_t)(Io8(0x48) & 0x3F), (uint8_t)(Io8(0x49) & 0x3F), (uint8_t)(Io8(0x4A) & 0x3F) };
    int v[2][2], en[2] = { w0, w1 };
    for (int i = 0; i < 2; i++)
    {
        int vv = Io16(0x44 + i * 2), t = vv >> 8, b = vv & 255;
        if (b > 160 || t > b) b = 160;
        v[i][0] = t; v[i][1] = b;
    }
    /* valores por linha de WIN0H/WIN1H: registrador, ou callback de HBlank, ou DMA de HBlank */
    static uint16_t hv[2][160];
    int cb = gHBlankCallback && (Io16(4) & 0x10);
    for (int y = 0; y < 160; y++)
    {
        if (cb) { *(uint16_t *)(gGbaIo + 6) = (uint16_t)y; gHBlankCallback(); }
        for (int i = 0; i < 2; i++)
        {
            uint16_t x = Io16(0x40 + i * 2);
            PlatformHblankValue((uintptr_t)gGbaIo + 0x40 + i * 2, y, &x);
            hv[i][y] = x;
        }
    }
    if (cb) *(uint16_t *)(gGbaIo + 6) = 0;
    int y0 = 0;
    for (int y = 1; y <= 160; y++)
    {
        int same = y < 160;
        if (same)
            for (int i = 0; i < 2; i++)
            {
                int a = en[i] && y >= v[i][0] && y < v[i][1], b = en[i] && (y - 1) >= v[i][0] && (y - 1) < v[i][1];
                if (a != b || (a && hv[i][y] != hv[i][y - 1])) same = 0;
            }
        if (same) continue;
        /* faixa [y0, y) com a mesma configuracao */
        int ly = y0, l[2], r[2], in[2];
        for (int i = 0; i < 2; i++)
        {
            in[i] = en[i] && ly >= v[i][0] && ly < v[i][1];
            l[i] = hv[i][ly] >> 8; r[i] = hv[i][ly] & 255;
            if (r[i] > 240 || l[i] > r[i]) r[i] = 240;
            if (l[i] == 0) l[i] = -1000;
            if (r[i] >= 240) r[i] = 1000;
        }
        int xs[6], nx = 0; xs[nx++] = -1000; xs[nx++] = 1000;
        for (int i = 0; i < 2; i++) if (in[i]) { xs[nx++] = l[i]; xs[nx++] = r[i]; }
        for (int a = 1; a < nx; a++) { int t = xs[a], b = a - 1; while (b >= 0 && xs[b] > t) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = t; }
        for (int a = 0; a + 1 < nx; a++)
        {
            if (xs[a] == xs[a + 1] || sNCell >= MAXCELL) continue;
            int mid = (xs[a] + xs[a + 1]) / 2, m = mk[2], outside = 0;
            if (in[0] && mid >= l[0] && mid < r[0]) m = mk[0];
            else if (in[1] && mid >= l[1] && mid < r[1]) m = mk[1];
            else outside = 1;
            if (outside && wo) { EmitOutside(xs[a], xs[a + 1], y0, y, m, (uint8_t)(Io8(0x4B) & 0x3F)); continue; }
            sCell[sNCell].x0 = xs[a]; sCell[sNCell].x1 = xs[a + 1]; sCell[sNCell].y0 = y0; sCell[sNCell].y1 = y; sCell[sNCell].mask = (uint8_t)m;
            sNCell++;
        }
        y0 = y;
    }
}

static int WinAnyLayer(int layer)
{
    if (!sWinOn) return 1;
    for (int i = 0; i < sNCell; i++) if (sCell[i].mask & (1 << layer)) return 1;
    return 0;
}

/* BeQuad com recorte pelas janelas. layer: 0-3 = BG, 4 = OBJ */
static void Q(int layer, int tex, int clut, float x, float y, float w, float h, float u0, float v0, float u1, float v1, int alpha, int shade)
{
    if (!sWinOn) { BeQuad(tex, clut, x, y, w, h, u0, v0, u1, v1, alpha, shade); return; }
    for (int i = 0; i < sNCell; i++)
    {
        if (!(sCell[i].mask & (1 << layer))) continue;
        float cx0 = x > sCell[i].x0 ? x : sCell[i].x0, cx1 = x + w < sCell[i].x1 ? x + w : sCell[i].x1;
        float cy0 = y > sCell[i].y0 ? y : sCell[i].y0, cy1 = y + h < sCell[i].y1 ? y + h : sCell[i].y1;
        if (cx0 >= cx1 || cy0 >= cy1) continue;
        float ua = u0 + (u1 - u0) * (cx0 - x) / w, ub = u0 + (u1 - u0) * (cx1 - x) / w;
        float va = v0 + (v1 - v0) * (cy0 - y) / h, vb = v0 + (v1 - v0) * (cy1 - y) / h;
        int fx = sCell[i].mask & 0x20;
        BeQuad(tex, clut, cx0, cy0, cx1 - cx0, cy1 - cy0, ua, va, ub, vb, fx ? alpha : 128, fx ? shade : 128);
    }
}

static void DrawBgStrip(int bg, int alpha, int shade, int y0, int y1, int hofs, int vofs)
{
    BgLayerCache *L = &gBgLayer[bg];
    int W = L->wPx, H = L->hPx, clut = L->is8bpp ? CLUT_BG8 : CLUT_BG4;
    hofs &= W - 1; vofs &= H - 1;
    /* 16:9 nativo: so camadas largas (mapas de 512 px, os do gameplay) mostram as colunas extras; as de 256 px (menus, telas
     * fixas) repetiriam a imagem nas laterais, entao ficam so no centro */
    int pad = sWs;   /* gameplay: fundos de 256 px (parallax) se repetem e tambem enchem o 16:9; outras cenas: sem colunas extras */
    /* 16:9 nativo: alem das bordas da sala nao ha mapa carregado (a VRAM tem colunas antigas do outro lado do wrap de 512 px):
     * nas colunas extras fora da sala nao desenha nada (aparece a cor de fundo) */
    int xlo = -pad, xhi = 240 + pad;
    if (pad && gMainGameMode == 4 && bg < 3 && (((const uint8_t *)&gCurrentRoomEntry)[1 + bg] & 0x10) && gBgPointersAndDimensions.backgrounds[bg].width)
    {
        int P = (bg == 0 ? gBg0XPosition : bg == 1 ? gBg1XPosition : gBg2XPosition) / 4;
        int lo = -P, hi = gBgPointersAndDimensions.backgrounds[bg].width * 16 - P;
        if (lo > -pad && lo <= 0) xlo = lo;
        if (hi < 240 + pad && hi >= 240) xhi = hi;
    }
    /* o que e tile 100% transparente nao precisa de fill no GS: desenha em faixas de 1 tile de altura so os trechos com conteudo.
     * Camadas totalmente cheias (e sem tile vazio na area visivel) seguem no caminho de 1 quad. */
    int anyEmpty = 0;
#ifdef USE_SKIP_EMPTY
    for (int y = y0; y < y1 && !anyEmpty;)
    {
        int sy = (vofs + y) % H, ty = sy >> 3, hs = 8 - (sy & 7);
        for (int x = xlo; x < xhi && !anyEmpty; x += 8 - (((hofs + x) % W + W) % W & 7))
            anyEmpty = L->empty[ty * 64 + ((((hofs + x) % W + W) % W) >> 3)];
        y += hs;
    }
#endif
    if (!anyEmpty)
    {
        for (int y = y0; y < y1;)
        {
            int sy = (vofs + y) % H, hs = y1 - y < H - sy ? y1 - y : H - sy;
            for (int x = xlo; x < xhi;)
            {
                int sx = ((hofs + x) % W + W) % W, ws = xhi - x < W - sx ? xhi - x : W - sx;
                Q(bg, bg, clut, x, y, ws, hs, sx, sy, sx + ws, sy + hs, alpha, shade);
                x += ws;
            }
            y += hs;
        }
        return;
    }
    for (int y = y0; y < y1;)
    {
        int sy = (vofs + y) % H, ty = sy >> 3, hs = 8 - (sy & 7);
        if (hs > y1 - y) hs = y1 - y;
        int rx = 0, rsx = 0, rw = 0;   /* trecho aberto: x na tela, x na camada, largura */
        for (int x = xlo; x < xhi;)
        {
            int sx = ((hofs + x) % W + W) % W, tc = sx >> 3, seg = 8 - (sx & 7);
            if (seg > xhi - x) seg = xhi - x;
            int emp = L->empty[ty * 64 + tc];
            if (rw && (emp || tc == 0)) { Q(bg, bg, clut, rx, y, rw, hs, rsx, sy, rsx + rw, sy + hs, alpha, shade); rw = 0; }
            if (!emp) { if (!rw) { rx = x; rsx = sx; } rw += seg; }
            x += seg;
        }
        if (rw) Q(bg, bg, clut, rx, y, rw, hs, rsx, sy, rsx + rw, sy + hs, alpha, shade);
        y += hs;
    }
}

static void DrawBgAffine(int bg, int alpha, int shade)
{
    BgLayerCache *L = &gBgLayer[bg];
    int clut = CLUT_BG8; float W = (float)L->wPx, H = (float)L->hPx;
    int ro = 0x20 + (bg - 2) * 0x10;
    float pa = (int16_t)Io16(ro) / 256.0f, pb = (int16_t)Io16(ro + 2) / 256.0f, pc = (int16_t)Io16(ro + 4) / 256.0f, pd = (int16_t)Io16(ro + 6) / 256.0f;
    int32_t rx = (int32_t)(*(uint32_t *)(gGbaIo + ro + 8) << 4) >> 4, ry = (int32_t)(*(uint32_t *)(gGbaIo + ro + 12) << 4) >> 4;
    float X = rx / 256.0f, Y = ry / 256.0f;
    float px[10] = { 0, 240, 240, 0 }, py[10] = { 0, 0, 160, 160 };
    int n = 4;
    /* Sutherland-Hodgman contra 0<=u<=W e 0<=v<=H (u e v sao lineares em x,y) */
    for (int k = 0; k < 4 && n; k++)
    {
        float ox2[10], oy2[10]; int m = 0;
        for (int i = 0; i < n; i++)
        {
            int j = (i + 1) % n;
            float fi, fj;
            #define UF(x, y) (X + pa * ((x) - 0.5f) + pb * ((y) - 0.5f) + 0.5f)
            #define VF(x, y) (Y + pc * ((x) - 0.5f) + pd * ((y) - 0.5f) + 0.5f)
            switch (k)
            {
                case 0: fi = UF(px[i], py[i]); fj = UF(px[j], py[j]); break;
                case 1: fi = W - UF(px[i], py[i]); fj = W - UF(px[j], py[j]); break;
                case 2: fi = VF(px[i], py[i]); fj = VF(px[j], py[j]); break;
                default: fi = H - VF(px[i], py[i]); fj = H - VF(px[j], py[j]); break;
            }
            if (fi >= 0 && m < 9) { ox2[m] = px[i]; oy2[m] = py[i]; m++; }
            if ((fi >= 0) != (fj >= 0) && m < 9)
            {
                float t = fi / (fi - fj);
                ox2[m] = px[i] + (px[j] - px[i]) * t; oy2[m] = py[i] + (py[j] - py[i]) * t; m++;
            }
        }
        for (int i = 0; i < m; i++) { px[i] = ox2[i]; py[i] = oy2[i]; }
        n = m;
    }
    if (n < 3) return;
    for (int i = 1; i + 1 < n; i++)
    {
        float xs[4] = { px[0], px[i], px[i + 1], px[i + 1] }, ys[4] = { py[0], py[i], py[i + 1], py[i + 1] }, us[4], vs[4];
        for (int k = 0; k < 4; k++)
        {
            us[k] = UF(xs[k], ys[k]); vs[k] = VF(xs[k], ys[k]);
            if (us[k] < 0) us[k] = 0; if (us[k] > W) us[k] = W; if (vs[k] < 0) vs[k] = 0; if (vs[k] > H) vs[k] = H;
        }
        BeQuadAffine(bg, clut, xs, ys, us, vs, alpha, shade);
    }
}

/* scroll por linha: o jogo (haze/agua/calor) programa um DMA de HBlank que reescreve BGxHOFS/VOFS a cada scanline */
static void DrawBg(int bg, int alpha, int shade)
{
    if (gBgLayer[bg].affine) { if (WinAnyLayer(bg)) DrawBgAffine(bg, alpha, shade); return; }
    uintptr_t ah = (uintptr_t)gGbaIo + 0x10 + bg * 4, av = ah + 2;
    uint16_t h0, v0, h, v;
    int y0 = 0, perLine = 0;
    h0 = Io16(0x10 + bg * 4); v0 = Io16(0x12 + bg * 4);
    int hs = PlatformHblankValue(ah, 0, &h), vs = PlatformHblankValue(av, 0, &v);
    if (!hs && !vs) { DrawBgStrip(bg, alpha, shade, 0, 160, h0, v0); return; }
    (void)perLine;
    uint16_t ch = hs ? h : h0, cv = vs ? v : v0;
    for (int y = 1; y <= 160; y++)
    {
        uint16_t nh = h0, nv = v0;
        if (y < 160) { if (hs) PlatformHblankValue(ah, y, &nh); if (vs) PlatformHblankValue(av, y, &nv); }
        if (y == 160 || nh != ch || nv != cv)
        {
            DrawBgStrip(bg, alpha, shade, y0, y, ch, cv);
            y0 = y; ch = nh; cv = nv;
        }
    }
}

/* uma linha de tiles contiguos no atlas (quebra ao cruzar a borda da linha do atlas) */
static void ObjRow(int is8, int idx, int wT, int hf, int vf, float x, float y, int clut, int alpha, int shade)
{
    int perRow = is8 ? 16 : 32, mask = is8 ? 0x1FF : 0x3FF, s = 0;
    while (s < wT)
    {
        idx &= mask;
        int col = idx % perRow, len = wT - s < perRow - col ? wT - s : perRow - col;
        float u0 = col * 8, v0 = (idx / perRow) * 8, u1 = u0 + len * 8, v1 = v0 + 8;
        float sx = hf ? x + (wT - s - len) * 8 : x + s * 8;
        /* espelhado: o UV comeca no fim exclusivo do tile; o GS le exatamente nessa borda no 1o pixel e pega o tile vizinho do
         * atlas (coluna/linha solta na borda do sprite). 1/16 = a precisao do UV do GS. */
        if (hf) { float t = u0; u0 = u1 - 0.0625f; u1 = t - 0.0625f; }
        if (vf) { float t = v0; v0 = v1 - 0.0625f; v1 = t - 0.0625f; }
        Q(4, is8 ? TEX_OBJ8 : TEX_OBJ4, clut, sx, y, len * 8, 8, u0, v0, u1, v1, alpha, shade);
        idx += len; s += len;
    }
}

static void DrawObjs(int prio, int oneD)
{
    const uint16_t *oam = (const uint16_t *)gGbaOam;
    for (int i = 127; i >= 0; i--)
    {
        uint16_t a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        if (((a2 >> 10) & 3) != prio) continue;
        int aff = (a0 >> 8) & 1, dbl = (a0 >> 9) & 1, mode = (a0 >> 10) & 3;
        if (!aff && dbl) continue;       /* desligado */
        if (mode == 2) continue;         /* janela de OBJ: nao desenha, vira mascara (BuildObjMask) */
        int shape = a0 >> 14, sz = a1 >> 14;
        if (shape == 3) continue;
        int wT = kW[shape][sz] >> 3, hT = kH[shape][sz] >> 3, is8 = (a0 >> 13) & 1;
        int oy = a0 & 0xFF, ox = a1 & 0x1FF;
        if (ox >= 384) ox -= 512;    /* 9 bits com sinal; com o 16:9 as pecas em x=256..383 estao a direita da tela (nao sao negativas) */
        if (oy >= 160) oy -= 256;
        /* 16:9 nativo: o HUD vai para os cantos da tela larga (esquerda: energia/misseis; direita: minimapa) */
        if (i < gPortHudOamCount && gMainGameMode == 4 /* GM_INGAME */ && sWs) ox += ox < 120 ? -sWs : sWs;
        int bw = wT * 8 << (aff & dbl), bh = hT * 8 << (aff & dbl);
        int wsPad = sWs;
        if (ox >= 240 + wsPad || oy >= 160 || ox + bw <= -wsPad || oy + bh <= 0) continue;

        int alpha = 128, shade = 128, bl = 0;
        if (mode == 1 && (sSecond & sDrawn)) { alpha = sEva * 8; bl = 1; }
        else if (sEffect == 1 && (sFirst & 0x10) && (sSecond & sDrawn)) { alpha = sEva * 8; bl = 1; }
        if (sEffect == 3 && (sFirst & 0x10) && !sFullFade) shade = 128 - sEvy * 8;
        if (sEffect == 2 && (sFirst & 0x10) && !sFullFade) shade = 128 + sEvy * 7;
        if (!alpha) continue;
        BeBlendAdd(bl && sAdd);
        int base = a2 & 0x3FF, bank = a2 >> 12;
        int clut = is8 ? CLUT_OBJ8 : CLUT_OBJ4(bank);

        if (!aff)
        {
            int hf = (a1 >> 12) & 1, vf = (a1 >> 13) & 1;
            /* bilinear: cada linha de 8 px e um quad com o clamp da propria linha, entao a emenda entre linhas nao mistura os
             * texels vizinhos (listra visivel na Samus). Sprites de 2+ linhas de tiles vao numa textura continua e saem em 1 quad. */
            if (hT > 1 && BeFilterLinear() && BeScratchOk() && sScratch < MAX_SCRATCH)
            {
                static uint8_t sbuf[64 * 64];
                int wPx = wT * 8, hPx = hT * 8;
                GfxAssembleObj(is8, oneD, base, wT, hT, sbuf);
                BeScratch(sScratch, is8, wPx, hPx, sbuf);
                float u0 = 0, u1 = (float)wPx, v0 = 0, v1 = (float)hPx;
                if (hf) { u0 = wPx - 0.0625f; u1 = -0.0625f; }   /* mesmo truque dos tiles espelhados (ObjRow) */
                if (vf) { v0 = hPx - 0.0625f; v1 = -0.0625f; }
                Q(4, TEX_SCRATCH0 + sScratch, clut, ox, oy, wPx, hPx, u0, v0, u1, v1, alpha, shade);
                sScratch++;
                continue;
            }
            for (int ty = 0; ty < hT; ty++)
            {
                int sy = vf ? hT - 1 - ty : ty;
                int unit = oneD ? base + sy * wT * (is8 ? 2 : 1) : base + sy * 32;
                ObjRow(is8, is8 ? unit >> 1 : unit, wT, hf, vf, ox, oy + ty * 8, clut, alpha, shade);
            }
            continue;
        }

        /* afim: monta o sprite numa textura propria e desenha um quad transformado */
        if (sScratch >= MAX_SCRATCH || !WinAnyLayer(4)) continue;
        static uint8_t buf[64 * 64];
        int g = (a1 >> 9) & 31;
        float pa = (int16_t)oam[g * 16 + 3] / 256.0f, pb = (int16_t)oam[g * 16 + 7] / 256.0f;
        float pc = (int16_t)oam[g * 16 + 11] / 256.0f, pd = (int16_t)oam[g * 16 + 15] / 256.0f;
        float det = pa * pd - pb * pc;
        if (det > -1e-6f && det < 1e-6f) continue;
        float ia = pd / det, ib = -pb / det, ic = -pc / det, id = pa / det; /* inversa de M */
        int wPx = wT * 8, hPx = hT * 8;
        GfxAssembleObj(is8, oneD, base, wT, hT, buf);
        BeScratch(sScratch, is8, wPx, hPx, buf);
        float cx = ox + bw * 0.5f, cy = oy + bh * 0.5f;
        float tu[4] = {0, (float)wPx, 0, (float)wPx}, tv[4] = {0, 0, (float)hPx, (float)hPx};
        float sx[4], sy[4];
        for (int k = 0; k < 4; k++)
        {
            float dx = tu[k] - wPx * 0.5f, dy = tv[k] - hPx * 0.5f;
            sx[k] = cx + ia * dx + ib * dy;
            sy[k] = cy + ic * dx + id * dy;
        }
        BeQuadAffine(TEX_SCRATCH0 + sScratch, clut, sx, sy, tu, tv, alpha, shade);
        sScratch++;
    }
    BeBlendAdd(0);
}

#ifdef HBTEST
extern void PlatformDmaTrigger(volatile unsigned int *r);
static void HbTest(void)
{
    static uint16_t tab[4][160]; static int t;
    for (int bg = 0; bg < 4; bg++)
    {
        for (int i = 0; i < 160; i++) { int k = ((i + t) & 31); tab[bg][i] = Io16(0x10 + bg * 4) + (k < 16 ? k : 32 - k); }
        volatile unsigned int *r = (volatile unsigned int *)(gGbaIo + 0xB0 + bg * 12);
        r[0] = (unsigned)(uintptr_t)tab[bg]; r[1] = (unsigned)(uintptr_t)(gGbaIo + 0x10 + bg * 4); r[2] = ((0x8000u | 0x2000u | 0x200u) << 16) | 1;
        PlatformDmaTrigger(r);
    }
    t++;
}
#endif

#ifdef WINTEST
static void WinTest(void)
{
    *(uint16_t *)(gGbaIo + 0) |= 0x2000;
    *(uint16_t *)(gGbaIo + 0x40) = (60 << 8) | 180;
    *(uint16_t *)(gGbaIo + 0x44) = (40 << 8) | 120;
    gGbaIo[0x48] = 0x1F; gGbaIo[0x4A] = 0x00;
}
#endif

#ifdef AFFTEST
#include <math.h>
static void AffTest(void)
{
    static int t; t++;
    extern unsigned char gGbaVram[];
    for (int i = 0; i < 256; i++) for (int k = 0; k < 64; k++) gGbaVram[0x8000 + i * 64 + k] = (uint8_t)(((k + (k >> 3)) & 1) ? 1 + (i % 15) : 0);
    for (int ty = 0; ty < 32; ty++) for (int tx = 0; tx < 32; tx++) gGbaVram[30 * 0x800 + ty * 32 + tx] = (uint8_t)(tx + ty * 2);
    float a = t * 0.03f, sc = 1.0f + 0.5f * sinf(t * 0.02f);
    *(uint16_t *)(gGbaIo + 0) = 1 | (1 << 10) | (1 << 12);   /* modo 1, BG2, OBJ */
    *(uint16_t *)(gGbaIo + 0x0C) = (1u << 14) | (2 << 2) | (30 << 8) | (1 << 7);  /* BG2CNT: 256x256, char 2, screen 30 */
    int pa = (int)(cosf(a) * 256 / sc), pb = (int)(sinf(a) * 256 / sc);
    *(int16_t *)(gGbaIo + 0x20) = pa; *(int16_t *)(gGbaIo + 0x22) = pb; *(int16_t *)(gGbaIo + 0x24) = -pb; *(int16_t *)(gGbaIo + 0x26) = pa;
    int32_t x = (128 << 8) - pa * 120 - pb * 80, y = (128 << 8) + pb * 120 - pa * 80;
    *(int32_t *)(gGbaIo + 0x28) = x; *(int32_t *)(gGbaIo + 0x2C) = y;
}
#endif

void RenderFrame(void)
{
#ifdef AFFTEST
    AffTest();
#endif
#ifdef WINTEST
    WinTest();
#endif
#ifdef HBTEST
    HbTest();
#endif
    uint32_t t0 = BeCycles();
    GfxCacheUpdate();
    BeProfileMark(0, BeCycles() - t0);
    uint16_t dispcnt = Io16(0);
    { int safe = WsSafeScene(); sWs = safe ? BeWsPad() : 0; gRenderWsOn = safe; BeSetWsMask(BeWsPad() && !safe); }
    BeBeginFrame(*(uint16_t *)gGbaPalram);
    {   /* gradiente de fundo: o DMA de HBlank reescreve a cor 0 da paleta (backdrop) em cada linha */
        uint16_t c;
        if (PlatformHblankValue((uintptr_t)gGbaPalram, 0, &c))
        {
            int y0 = 0; uint16_t cur = c;
            for (int y = 1; y <= 160; y++)
            {
                uint16_t n = cur;
                if (y < 160) PlatformHblankValue((uintptr_t)gGbaPalram, y, &n);
                if (y == 160 || n != cur) { BeBackdropRows(y0, y, cur); y0 = y; cur = n; }
            }
        }
    }
    uint32_t t1 = BeCycles();
    BeSync();
    BeProfileMark(1, BeCycles() - t1);
    uint32_t t2 = BeCycles();

    if (!(dispcnt & 0x80))
    {
        BuildWindows(dispcnt);
        uint16_t bld = Io16(0x50), ba = Io16(0x52), by = Io16(0x54);
#ifdef TESTFX
        if (TESTFX == 1) { bld = 0x1F00 | (1 << 6) | 0x10 | 0x0F; ba = 8 | (16 << 8); }   /* aditivo: BGs+OBJ sobre o que esta atras */
        if (TESTFX == 2) { bld = (2 << 6) | 0x1F; by = 10; }                      /* clarear camadas */
        if (TESTFX == 3) { bld = 0x1F00 | (1 << 6) | 0x1F; ba = 8 | (8 << 8); }   /* normal 50% */
#endif
        sEffect = (bld >> 6) & 3; sFirst = bld & 0x3F; sSecond = (bld >> 8) & 0x3F;
        sEva = (ba & 31) > 16 ? 16 : (ba & 31); sEvy = (by & 31) > 16 ? 16 : (by & 31);
        sEvb = (ba >> 8 & 31) > 16 ? 16 : (ba >> 8 & 31);
        sAdd = sEvb == 16 && sEva + sEvb != 16;
        sFullFade = sEffect >= 2 && sFirst == 0x3F;
        sDrawn = 0x20; sScratch = 0;

        for (int prio = 3; prio >= 0; prio--)
        {
            for (int bg = 3; bg >= 0; bg--)
            {
                if (!gBgLayer[bg].enabled || (Io16(0x08 + bg * 2) & 3) != prio) continue;
                int alpha = 128, shade = 128, bit = 1 << bg, bl = 0;
                if (sEffect == 1 && (sFirst & bit) && (sSecond & sDrawn)) { alpha = sEva * 8; bl = 1; }
                if (sEffect == 3 && (sFirst & bit) && !sFullFade) shade = 128 - sEvy * 8;
                if (sEffect == 2 && (sFirst & bit) && !sFullFade) shade = 128 + sEvy * 7;   /* aproximacao: modula ate ~2x e satura */
                if (alpha) { BeBlendAdd(bl && sAdd); DrawBg(bg, alpha, shade); BeBlendAdd(0); }
                sDrawn |= bit;
            }
            if (dispcnt & 0x1000) { DrawObjs(prio, (dispcnt >> 6) & 1); sDrawn |= 0x10; }
        }
        if (sFullFade && sEvy)
        {
            if (!sWinOn) BeOverlay(sEffect == 2, sEvy * 8);
            else
                for (int i = 0; i < sNCell; i++)
                    if (sCell[i].mask & 0x20)
                    {
                        int pad = sWs, x0 = sCell[i].x0 < -pad ? -pad : sCell[i].x0, x1 = sCell[i].x1 > 240 + pad ? 240 + pad : sCell[i].x1;
                        BeOverlayRect(x0, sCell[i].y0, x1 - x0, sCell[i].y1 - sCell[i].y0, sEffect == 2, sEvy * 8);
                    }
        }
    }
    BeProfileMark(2, BeCycles() - t2);
    BeEndFrame();
}
