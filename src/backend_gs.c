/* Backend do GS (gsKit): recebe quads texturizados e os desenha com o hardware. */
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsHires.h>
#include <math.h>
#include "gfx_cache.h"
#include "gfx_backend.h"

#define GBA_W 240
#define GBA_H 160
#define NUM_CLUTS (3 + 16)
#define MS(c) ((c) / 294912.0f)

static uint32_t prof[8], profAcc[8], profSnap[8], profAccP, profSnapP, profAccN, profLastEnd, profFrozen;
/* slots: 0 cache de camadas, 1 upload de texturas, 2 montagem do desenho, 3 GS+vsync, 4 logica do jogo, 5 sistema (audio/save/vblank cb) */
uint32_t BeCycles(void) { uint32_t c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
void BeProfileMark(int slot, uint32_t cycles) { prof[slot] = cycles; }
void BeProfileFreeze(int f) { profFrozen = f; }
float BeProfileMs(int slot) { return MS(profSnap[slot]); }
float BeProfilePeriodMs(void) { return MS(profSnapP); }
/* media de 30 quadros; congelada enquanto o menu esta aberto (o menu nao e o jogo) */
static void ProfileFrame(void)
{
    uint32_t now = BeCycles(), per = now - profLastEnd;
    profLastEnd = now;
    if (profFrozen) return;
    for (int i = 0; i < 8; i++) profAcc[i] += prof[i];
    profAccP += per;
    if (++profAccN >= 30)
    {
        for (int i = 0; i < 8; i++) { profSnap[i] = profAcc[i] / profAccN; profAcc[i] = 0; }
        profSnapP = profAccP / profAccN; profAccP = profAccN = 0;
    }
}

int gBeDbg[5] = {1, 1, 1, 1, 1}; /* diagnostico: 0 BG, 1 OBJ, 2 afins, 3 overlays */
static GSGLOBAL *gs;
static GSTEXTURE tex[TEX_SCRATCH0 + MAX_SCRATCH];
static u32 clutVram[NUM_CLUTS];
static u8 *scratchMem[MAX_SCRATCH];
static float ox, oy, SX = 2.5f, SY = 2.5f;       /* origem e escala do jogo, em unidades do framebuffer */
static float uiX, uiY, uiW = 320, uiH = 224;     /* area onde o menu (320x224) e desenhado */
static int sPerfBars = 0;
static int sMode = 1;     /* 0 = 160p, 1 = 480i, 2 = 480p, 3 = 1080i (hires) */
static int sHires = 0;
static int sAspect = 0;   /* so no 480p/1080i: 0 = 16:9 (estica), 1 = 4:3, 2 = 3:2 */
#ifndef CLAMP_FORCE
#define CLAMP_FORCE 1
#endif
static int sFilter = 0;   /* 0 = vizinho mais proximo, 1 = bilinear (fundo) */
static int sFilterSel = 0; /* menu: 0 NEAREST, 1 BILINEAR, 2 BILINEAR + sobreposicao das pecas, 3 so o fundo em bilinear (sprites nitidos) */
static int sObjLinear = 0, sObjPad = 0;
static int sColorFx = 0;   /* 1 = correcao de cor do GBA (paletas) */
static int sScreenFx = 0;  /* 0 nada, 1 LCD (linhas finas), 2 scanlines. */
static int sSizeSel = 0;
static int sPad = 0;           /* 16:9 nativo: colunas extras de cada lado (em pixels do GBA) */
#define WS_PAD 22      /* 160 * 16/9 = 284 -> (284 - 240) / 2 */
static int sForceUpload = 0;   /* VRAM refeita (troca de modo): reenviar tudo nos proximos quadros */
static void (*sUiHook)(void);

static void Layout(int size)
{
    sSizeSel = size;
    float fw = (float)gs->Width, fh = (float)gs->Height;
    uiX = 0; uiY = 0; uiW = fw; uiH = fh;
    sPad = 0;
    if (sMode == 0)            /* 160p: o jogo 1:1 (240x160) no framebuffer de 256x160 */
    {
        SX = SY = 1.0f;
        ox = (fw - GBA_W) * 0.5f; oy = 0;
    }
    else if (sMode >= 2 || (sMode == 1 && sAspect == 0))       /* 480p (704x480) e 1080i (1920x540, cada unidade vertical = 2 linhas): tela cheia */
    {
        /* aspecto: 0 = 16:9 NATIVO (mostra 22 colunas a mais de cada lado), 1 = 4:3, 2 = 3:2, 3 = 16:9 esticado */
        static const float frac[4] = {1.0f, 0.75f, 0.84375f, 1.0f};
        float dispW = fw * frac[sAspect];
        sPad = sAspect == 0 ? WS_PAD : 0;
        SX = dispW / (GBA_W + 2 * sPad); SY = fh / GBA_H;
        ox = (fw - dispW) * 0.5f + sPad * SX; oy = 0;
        uiX = (fw - dispW) * 0.5f; uiW = dispW;
        if (sPad) { uiW = fw * 0.75f; uiX = (fw - uiW) * 0.5f; }   /* menu em 4:3 no centro */
        if (sMode == 3) { ox -= 0.5f; oy += -0.25f; }   /* deslocamento do OPL/Super Metroid: alinha as passadas e os dois campos do 1080i */
    }
    else                       /* 480i: resolucao nativa do PS2 */
    {
        float fit = fw / GBA_W, f2 = fh / GBA_H;
        if (f2 < fit) fit = f2;
        SX = SY = size == 0 ? 2.0f : size == 1 ? 2.5f : fit;
        ox = (fw - GBA_W * SX) * 0.5f;
        oy = (fh - GBA_H * SY) * 0.5f;
    }
}

static void ApplyFilter(void)
{
    sObjLinear = sFilterSel == 1 || sFilterSel == 2;
    sObjPad = sFilterSel == 2;
    for (int i = 0; i < TEX_SCRATCH0 + MAX_SCRATCH; i++)
    {
        int lin = i < 4 ? sFilter : sObjLinear;   /* 0-3 = camadas de fundo; o resto = sprites */
        tex[i].Filter = lin ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    }
}

int BeWsPad(void) { return sPad; }
static int sWsMask;
void BeSetWsMask(int on) { sWsMask = on; }
void BeSetScreenFx(int fx) { sScreenFx = fx; }

/* Correcao de cor do GBA: a tela do GBA era escura e pouco saturada. Linear (gama 3.2) -> mistura de canais -> gama de saida 1/2.2.
 * Aplicada nas paletas ao enviar ao GS (so quando a opcao esta ligada). */
static u8 sGbaOut[1025];   /* linear 0..1 (x1024) -> 5 bits */
static float sGbaLin[32];
static void GbaColorInit(void)
{
    if (sGbaOut[1024]) return;
    for (int i = 0; i < 32; i++) sGbaLin[i] = powf(i / 31.0f, 3.2f);
    for (int i = 0; i <= 1024; i++) { float v = powf(i / 1024.0f, 1.0f / 2.2f) * 31.0f + 0.5f; sGbaOut[i] = (u8)(v > 31 ? 31 : v); }
    sGbaOut[1024] = 31;
}
static const u16 *ClutFx(const u16 *src, int n, int slot)
{
    static u16 buf[21][256] __attribute__((aligned(64)));   /* o DMA do GS exige origem alinhada em 16 bytes */
    if (!sColorFx) return src;
    GbaColorInit();
    u16 *d = buf[slot];
    for (int i = 0; i < n; i++)
    {
        u16 c = src[i];
        float r = sGbaLin[c & 31], g = sGbaLin[(c >> 5) & 31], b = sGbaLin[(c >> 10) & 31];
        float R = 0.84f * r + 0.12f * g + 0.04f * b, G = 0.14f * r + 0.78f * g + 0.08f * b, B = 0.05f * r + 0.18f * g + 0.77f * b;
        d[i] = (u16)((c & 0x8000) | (sGbaOut[(int)(B * 1024.0f)] << 10) | (sGbaOut[(int)(G * 1024.0f)] << 5) | sGbaOut[(int)(R * 1024.0f)]);
    }
    return d;
}
void BeSetColorFx(int on) { if (sColorFx != (on != 0)) { sColorFx = on != 0; sForceUpload = 4; } }
void BeSetUiHook(void (*hook)(void)) { sUiHook = hook; }
/* coordenadas do jogo (pixels do GBA, pode ser negativo no 16:9 nativo) -> coordenadas da interface (320x224) */
void BeGameToUi(float gx, float gy, int *ux, int *uy)
{
    *ux = (int)((ox + gx * SX - uiX) * 320.0f / uiW);
    *uy = (int)((oy + gy * SY - uiY) * 224.0f / uiH);
}

void BeUiRect(int x, int y, int w, int h, int r, int g, int b, int alpha)
{
    float sx = uiW / 320.0f, sy = uiH / 224.0f;
    gsKit_prim_sprite(gs, uiX + x * sx, uiY + y * sy, uiX + (x + w) * sx, uiY + (y + h) * sy, 2, GS_SETREG_RGBAQ(r, g, b, alpha, 0));
}

static u32 VramAlloc(int w, int h, int psm)
{
    u32 a = gsKit_vram_alloc(gs, gsKit_texture_size(w, h, psm), GSKIT_ALLOC_USERBUFFER);
    if (a == GSKIT_ALLOC_ERROR) printf("mzm: sem VRAM do GS (%dx%d psm %d)\n", w, h, psm);
    return a;
}

static void InitTex(int id, int w, int h, int psm, int tbw, void *mem)
{
    GSTEXTURE *t = &tex[id];
    memset(t, 0, sizeof *t);
    t->Width = w; t->Height = h; t->PSM = psm; t->TBW = tbw;
    t->ClutPSM = GS_PSM_CT16; t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
    t->Filter = sFilter ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    t->Mem = mem;
    t->Vram = VramAlloc(w, h, psm);
}

static void VideoTeardown(void)
{
    if (!gs) return;
    if (sHires) gsKit_hires_deinit_global(gs); else gsKit_deinit_global(gs);
    gs = NULL;
}

/* desmonta o video (handlers de vblank/DMA do gsKit) antes de entrar em outro programa */
void BeShutdown(void) { VideoTeardown(); }

static void VideoSetup(int mode)
{
    int pal = (gsKit_check_rom() == GS_MODE_PAL);
    static int dmaReady;
    sMode = mode;
    sHires = (mode >= 2);   /* 480p e 1080i usam o modo multi-passada (hires) do gsKit, como o OPL */
    if (!dmaReady)
    {
        dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
        dmaKit_chan_init(DMA_CHANNEL_GIF);
        dmaReady = 1;
    }
    gs = sHires ? gsKit_hires_init_global() : gsKit_init_global();
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->Dithering = GS_SETTING_OFF;
    if (mode >= 2)
    {
        gs->PrimAlphaEnable = GS_SETTING_ON;
        gs->DoubleBuffering = GS_SETTING_ON;
        gs->Dithering = GS_SETTING_ON;
        if (mode == 2)
        {
            /* igual ao OPL "DTV480P 704x480 full screen": progressivo, 24 bits, 2 passadas */
            gs->Mode = GS_MODE_DTV_480P;
            gs->Width = 704; gs->Height = 480;
            gs->Interlace = GS_NONINTERLACED; gs->Field = GS_FRAME;
            gs->PSM = GS_PSM_CT24;
            gs->OffsetX = ((4096 - gs->Width) / 2) * 16;
            gs->OffsetY = ((4096 - gs->Height) / 2) * 16;
            gsKit_hires_init_screen(gs, 2);
        }
        else
        {
            /* igual ao Super Metroid PS2 / OPL: 1920x1080 entrelacado em "frame", cor de 16 bits, 3 passadas */
            gs->Mode = GS_MODE_DTV_1080I;
            gs->Width = 1920; gs->Height = 1080;
            gs->Interlace = GS_INTERLACED; gs->Field = GS_FRAME;
            gs->PSM = GS_PSM_CT16S;
            gs->Height /= 2;
            gs->OffsetX = ((4096 - gs->Width) / 2) * 16;
            gs->OffsetY = ((4096 - gs->Height) / 2) * 16;
            gsKit_hires_init_screen(gs, 3);
        }
        gsKit_set_test(gs, GS_ZTEST_OFF);
    }
    else
    {
        gs->Mode = gsKit_check_rom();
        gs->PSM = GS_PSM_CT24;
        gs->DoubleBuffering = GS_SETTING_ON;
        if (mode == 0)        /* 160p: progressivo, framebuffer 256x160 (largura multipla de 64) */
        {
            gs->Mode = pal ? GS_MODE_PAL : GS_MODE_NTSC;
            gs->Width = 256; gs->Height = 160;
            gs->Interlace = GS_NONINTERLACED; gs->Field = GS_FRAME;
        }
        gsKit_init_screen(gs);
        gsKit_mode_switch(gs, GS_ONESHOT);
    }
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0); /* (Cs-Cd)*As + Cd */
    gs->PrimAlphaEnable = GS_SETTING_ON;

    for (int i = 0; i < 4; i++) InitTex(i, LAYER_STRIDE, LAYER_STRIDE, GS_PSM_T8, LAYER_STRIDE / 64, gBgLayer[i].pix);
    InitTex(TEX_OBJ4, 256, 256, GS_PSM_T4, 4, gObjAtlas4);
    InitTex(TEX_OBJ8, 128, 256, GS_PSM_T8, 2, gObjAtlas8);
    for (int i = 0; i < MAX_SCRATCH; i++)
    {
        if (!scratchMem[i]) scratchMem[i] = memalign(128, 64 * 64);
        InitTex(TEX_SCRATCH0 + i, 64, 64, GS_PSM_T8, 1, scratchMem[i]);
    }
    clutVram[CLUT_BG4] = VramAlloc(16, 16, GS_PSM_CT16);
    clutVram[CLUT_BG8] = VramAlloc(16, 16, GS_PSM_CT16);
    clutVram[CLUT_OBJ8] = VramAlloc(16, 16, GS_PSM_CT16);
    for (int b = 0; b < 16; b++) clutVram[CLUT_OBJ4(b)] = VramAlloc(8, 2, GS_PSM_CT16);

    /* a VRAM foi refeita: tudo precisa ser reenviado */
    sForceUpload = sHires ? 6 : 2;
    for (int i = 0; i < 4; i++) gBgLayer[i].dirty = 1;

    Layout(sSizeSel);
    if (sHires) { gsKit_hires_sync(gs); gsKit_hires_flip(gs); }
    else { gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0)); gsKit_sync_flip(gs); }
}

void BeInit(void) { VideoSetup(1); }

/* modo: 0 = 160p, 1 = 480i, 2 = 480p, 3 = 1080i; aspecto (480p/1080i): 0 = 16:9, 1 = 4:3, 2 = 3:2; filtro: 0 = nitido, 1 = bilinear */
void BeSetVideoMode(int mode, int aspect, int filter)
{
    sAspect = aspect; sFilterSel = filter; sFilter = filter != 0;
    if (mode != sMode) { VideoTeardown(); VideoSetup(mode); }
    ApplyFilter();
    Layout(sSizeSel);
}
void BeSetVideo(int size, int perfBars) { Layout(size); sPerfBars = perfBars; }

static int sStageIdx, sStageOff;
/* so reemite o registrador CLAMP quando o valor muda (cada gsKit_set_clamp empilha um pacote na fila) */
static int sClMode = -1, sClMinU, sClMaxU, sClMinV, sClMaxV;
static void SetClamp(int mode)
{
    if (sHires) { gsKit_set_clamp(gs, mode); return; }   /* multi-passada: o estado nao e continuo entre as filas */
    if (mode == sClMode && (mode != GS_CMODE_REGION_CLAMP ||
        (gs->Clamp->MINU == sClMinU && gs->Clamp->MAXU == sClMaxU && gs->Clamp->MINV == sClMinV && gs->Clamp->MAXV == sClMaxV))) return;
    sClMode = mode; sClMinU = gs->Clamp->MINU; sClMaxU = gs->Clamp->MAXU; sClMinV = gs->Clamp->MINV; sClMaxV = gs->Clamp->MAXV;
    gsKit_set_clamp(gs, mode);
}
void BeBeginFrame(uint16_t bd)
{
    sStageOff = 0;
    sClMode = -1;   /* estado do GS desconhecido no inicio do frame */
    if (!sFilter) SetClamp(GS_CMODE_REPEAT);
    if (sHires) gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0); /* cada fila de desenho precisa do seu ALPHA */
    if (sHires) gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 1, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    else gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_prim_sprite(gs, ox - sPad * SX, oy, ox + (GBA_W + sPad) * SX, oy + GBA_H * SY, 1,
        GS_SETREG_RGBAQ((bd & 31) << 3, ((bd >> 5) & 31) << 3, ((bd >> 10) & 31) << 3, 0x80, 0));
}

void BeBackdropRows(int y0, int y1, uint16_t bd)
{
    gsKit_prim_sprite(gs, ox - sPad * SX, oy + y0 * SY, ox + (GBA_W + sPad) * SX, oy + y1 * SY, 1,
        GS_SETREG_RGBAQ((bd & 31) << 3, ((bd >> 5) & 31) << 3, ((bd >> 10) & 31) << 3, 0x80, 0));
}

/* no 1080i (hires) a lista de desenho e reenviada a cada passada: os uploads entram na lista (inline) */
#ifndef HIRES_DIRECT
#define HIRES_DIRECT 0
#endif
/* No hires a fila de desenho e reenviada a cada passada, com os uploads apontando para a memoria de origem (DMA "REF").
 * Se a CPU alterar essa memoria (proximo quadro) no meio das passadas, cada faixa da tela mostra um quadro diferente
 * (degraus horizontais). Por isso o hires copia cada upload para uma area propria do quadro (2 areas alternadas). */
#define STAGE_SIZE (1536 * 1024)
static u8 sStage[2][STAGE_SIZE] __attribute__((aligned(64)));
static int sStageIdx, sStageOff;
static void Send(void *mem, int w, int h, u32 tbp, u32 psm, u32 tbw, u8 clut)
{
    if (sHires && !HIRES_DIRECT)
    {
        u32 bytes = (gsKit_texture_size_ee(w, h, psm) + 63) & ~63u;
        if (sStageOff + bytes > STAGE_SIZE) return;   /* nao deve acontecer: o pior caso cabe com folga */
        void *p = sStage[sStageIdx] + sStageOff;
        sStageOff += bytes;
        memcpy(p, mem, gsKit_texture_size_ee(w, h, psm));
        gsKit_texture_send_inline(gs, (u32 *)p, w, h, tbp, psm, tbw, clut);
    }
    else gsKit_texture_send((u32 *)mem, w, h, tbp, psm, tbw, clut);
}

/* No 1080i a lista de desenho e alternada entre duas filas e reenviada por passada: um upload so vale se estiver
 * em varias filas seguidas. Por isso, no hires, cada "sujo" gera REP reenvios nos quadros seguintes. */
#define HIRES_REP 4
void BeSync(void)
{
    static int repPal, repObj, repBg[4];
    FlushCache(0);
    if (!gBeDbg[4]) { gPalDirty = 0; gObjDirty = 0; for (int i = 0; i < 4; i++) gBgLayer[i].dirty = 0; return; }
    int pal = gPalDirty, obj = gObjDirty;
    if (sForceUpload > 0) { sForceUpload--; pal = obj = 1; for (int i = 0; i < 4; i++) gBgLayer[i].dirty = 1; }
    if (sHires)
    {
        if (pal) repPal = HIRES_REP;
        if (obj) repObj = HIRES_REP;
        pal = repPal > 0; obj = repObj > 0;
        if (pal) repPal--;
        if (obj) repObj--;
    }
    if (pal)
    {
        Send((void *)ClutFx((const u16 *)gClutBg4, 256, 0), 16, 16, clutVram[CLUT_BG4], GS_PSM_CT16, 1, GS_CLUT_PALLETE);
        Send((void *)ClutFx((const u16 *)gClutBg8, 256, 1), 16, 16, clutVram[CLUT_BG8], GS_PSM_CT16, 1, GS_CLUT_PALLETE);
        Send((void *)ClutFx((const u16 *)gClutObj8, 256, 2), 16, 16, clutVram[CLUT_OBJ8], GS_PSM_CT16, 1, GS_CLUT_PALLETE);
        for (int b = 0; b < 16; b++)
            Send((void *)ClutFx((const u16 *)gClutObj4[b], 16, 5 + b), 8, 2, clutVram[CLUT_OBJ4(b)], GS_PSM_CT16, 1, GS_CLUT_PALLETE);
    }
    if (obj)
    {
        Send(tex[TEX_OBJ4].Mem, 256, 256, tex[TEX_OBJ4].Vram, GS_PSM_T4, 4, GS_CLUT_NONE);
        Send(tex[TEX_OBJ8].Mem, 128, 256, tex[TEX_OBJ8].Vram, GS_PSM_T8, 2, GS_CLUT_NONE);
    }
    for (int i = 0; i < 4; i++)
    {
        BgLayerCache *L = &gBgLayer[i];
        int up = L->enabled && L->dirty;
        if (sHires)
        {
            if (up) repBg[i] = HIRES_REP;
            up = L->enabled && repBg[i] > 0;
            if (up) repBg[i]--;
        }
        if (!up) continue;
        Send(tex[i].Mem, LAYER_STRIDE, L->hPx, tex[i].Vram, GS_PSM_T8, LAYER_STRIDE / 64, GS_CLUT_NONE);
        L->dirty = 0;
    }
    FlushCache(0);   /* tambem fora do hires: ClutFx escreve nos buffers de paleta (cache) DEPOIS do flush do inicio; o DMA le a RAM */
}

int BeFilterLinear(void) { return sObjLinear; }   /* os sprites usam bilinear */
int BeScratchOk(void) { return gBeDbg[2]; }

void BeScratch(int slot, int is8, int wPx, int hPx, const uint8_t *data)
{
    if (!gBeDbg[2]) return;
    GSTEXTURE *t = &tex[TEX_SCRATCH0 + slot];
    int bytes = wPx * hPx / (is8 ? 1 : 2);
    memcpy(scratchMem[slot], data, bytes);
    FlushCache(0);
    t->Width = wPx; t->Height = hPx;
    t->PSM = is8 ? GS_PSM_T8 : GS_PSM_T4;
    t->TBW = (wPx + 63) / 64;
    Send(t->Mem, wPx, hPx, t->Vram, t->PSM, t->TBW, GS_CLUT_NONE);
    if (sHires) FlushCache(0);
}

static GSTEXTURE *Bind(int id, int clut)
{
    GSTEXTURE *t = &tex[id];
    t->Clut = (u32 *)gClutBg4; /* so precisa ser nao-nulo; o CLUT ja esta na VRAM do GS */
    t->VramClut = clutVram[clut];
    return t;
}

static int sAddBlend;
void BeBlendAdd(int on)
{
    if (on == sAddBlend) return;
    sAddBlend = on;
    /* EVB==16: Cs*As + Cd (soma, com saturacao) ; normal: (Cs-Cd)*As + Cd */
    gsKit_set_primalpha(gs, on ? GS_SETREG_ALPHA(0, 2, 0, 1, 0) : GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
}

#define COL(alpha, shade) GS_SETREG_RGBAQ(shade, shade, shade, alpha, 0)

/* Bilinear le texels vizinhos: sem isto os quads vazam para o que esta ao lado na textura (cortes/linhas).
 * Prende as coordenadas a regiao do proprio quad. */
static void ClampRegion(const float *u, const float *v, int n, int force)
{
    if (!sFilter && !force) return;
    float a = u[0], b = u[0], c = v[0], d = v[0];
    for (int i = 1; i < n; i++) { if (u[i] < a) a = u[i]; if (u[i] > b) b = u[i]; if (v[i] < c) c = v[i]; if (v[i] > d) d = v[i]; }
    /* tolerancia de 1/8 texel: UVs espelhados chegam como (borda - 1/16) e floorf cairia no texel anterior (-1 = 1023 em 10 bits,
     * regiao invertida = retangulo solido). */
    int mnu = (int)floorf(a + 0.125f), mxu = (int)ceilf(b - 0.125f) - 1;
    int mnv = (int)floorf(c + 0.125f), mxv = (int)ceilf(d - 0.125f) - 1;
    if (mnu < 0) mnu = 0; if (mnv < 0) mnv = 0;
    if (mxu > 1023) mxu = 1023; if (mxv > 1023) mxv = 1023;
    gs->Clamp->MINU = mnu; gs->Clamp->MAXU = mxu;
    gs->Clamp->MINV = mnv; gs->Clamp->MAXV = mxv;
    if (gs->Clamp->MAXU < gs->Clamp->MINU) gs->Clamp->MAXU = gs->Clamp->MINU;
    if (gs->Clamp->MAXV < gs->Clamp->MINV) gs->Clamp->MAXV = gs->Clamp->MINV;
    SetClamp(GS_CMODE_REGION_CLAMP);
}

void BeQuad(int id, int clut, float x, float y, float w, float h, float u0, float v0, float u1, float v1, int alpha, int shade)
{
    if (!gBeDbg[id < 4 ? 0 : 1]) return;
    { float uu[2] = {u0, u1}, vv[2] = {v0, v1}; ClampRegion(uu, vv, 2, CLAMP_FORCE); }   /* sempre: com escala fracionaria o GS le 1 texel alem da borda (linha fina) */
    if (sObjPad && id >= TEX_OBJ4)
    {
        /* sobreposicao: o quad cresce 'p' pixel do GBA para fora e o UV acompanha; o clamp da regiao original repete a borda */
        const float p = 0.5f;
        float du = (u1 - u0) / w, dv = (v1 - v0) / h;
        x -= p; y -= p; w += 2 * p; h += 2 * p;
        u0 -= du * p; u1 += du * p; v0 -= dv * p; v1 += dv * p;
    }
    gsKit_prim_sprite_texture(gs, Bind(id, clut),
        ox + x * SX, oy + y * SY, u0, v0,
        ox + (x + w) * SX, oy + (y + h) * SY, u1, v1, 1, COL(alpha, shade));
    if (!sFilter) SetClamp(GS_CMODE_REPEAT);
}

void BeQuadAffine(int id, int clut, const float *x, const float *y, const float *u, const float *v, int alpha, int shade)
{
    if (!gBeDbg[2]) return;
    /* sprites afins: o GS amostra um pouco alem da borda (precisao 1/16 de texel) e lia colunas/linhas velhas ou do lado
     * oposto da textura de 64x64 (REPEAT) = tracos finos atravessando o sprite. Sempre prender a regiao real. */
    ClampRegion(u, v, 4, 1);
    gsKit_prim_quad_texture(gs, Bind(id, clut),
        ox + x[0] * SX, oy + y[0] * SY, u[0], v[0],
        ox + x[1] * SX, oy + y[1] * SY, u[1], v[1],
        ox + x[2] * SX, oy + y[2] * SY, u[2], v[2],
        ox + x[3] * SX, oy + y[3] * SY, u[3], v[3], 1, COL(alpha, shade));
    if (!sFilter) SetClamp(GS_CMODE_REPEAT);   /* os quads comuns esperam o clamp padrao */
}

void BeOverlay(int white, int alpha)
{
    if (!gBeDbg[3]) return;
    int c = white ? 0x80 : 0;
    gsKit_prim_sprite(gs, ox - sPad * SX, oy, ox + (GBA_W + sPad) * SX, oy + GBA_H * SY, 1, GS_SETREG_RGBAQ(c, c, c, alpha, 0));
}

void BeOverlayRect(float x, float y, float w, float h, int white, int alpha)
{
    if (!gBeDbg[3]) return;
    int c = white ? 0x80 : 0;
    gsKit_prim_sprite(gs, ox + x * SX, oy + y * SY, ox + (x + w) * SX, oy + (y + h) * SY, 1, GS_SETREG_RGBAQ(c, c, c, alpha, 0));
}

/* efeitos de tela sobre a imagem do jogo (linhas escuras alinhadas aos pixels do GBA, em posicoes inteiras do framebuffer) */
static void DrawScreenFx(float x0, float x1)
{
    if (!sScreenFx || sMode == 0) return;
    float y0 = oy, y1 = oy + GBA_H * SY;
    if (sScreenFx == 2)   /* scanline: a parte de baixo de cada linha do GBA escurece */
    {
        u64 k = GS_SETREG_RGBAQ(0, 0, 0, 0x48, 0);
        for (int r = 0; r < GBA_H; r++)
        {
            float a = floorf(oy + (r + 0.55f) * SY + 0.5f), b = floorf(oy + (r + 1) * SY + 0.5f);
            if (b > a) gsKit_prim_sprite(gs, x0, a, x1, b, 1, k);
        }
    }
    else                  /* LCD: linha fina escura entre os pixels (horizontais e verticais) */
    {
        u64 k = GS_SETREG_RGBAQ(0, 0, 0, 0x30, 0);
        for (int r = 1; r <= GBA_H; r++)
        {
            float b = floorf(oy + r * SY + 0.5f);
            gsKit_prim_sprite(gs, x0, b - 1, x1, b, 1, k);
        }
        for (int c = -sPad + 1; c <= GBA_W + sPad; c++)
        {
            float b = floorf(ox + c * SX + 0.5f);
            gsKit_prim_sprite(gs, b - 1, y0, b, y1, 1, k);
        }
    }
}

void BeEndFrame(void)
{
    ProfileFrame();
    /* faixas pretas por cima: escondem tudo que passou dos 240x160 (sprites parcialmente fora) */
    u64 k = GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0);
    float x0 = ox - sPad * SX, x1 = ox + (GBA_W + sPad) * SX, y1 = oy + GBA_H * SY;
    if (sWsMask) { x0 = ox; x1 = ox + GBA_W * SX; }   /* cena sem widescreen: pilarbox preto */
    DrawScreenFx(x0, x1);
    gsKit_prim_sprite(gs, 0, 0, gs->Width, oy, 1, k);
    gsKit_prim_sprite(gs, 0, y1, gs->Width, gs->Height, 1, k);
    gsKit_prim_sprite(gs, 0, oy, x0, y1, 1, k);
    gsKit_prim_sprite(gs, x1, oy, gs->Width, y1, 1, k);
    /* barras de perfil (frame anterior): 30 px por ms; marca branca em 16,7 ms = 1 frame */
    static const u64 cols[4] = {GS_SETREG_RGBAQ(0xE0, 0x30, 0x30, 0x80, 0), GS_SETREG_RGBAQ(0xE0, 0xD0, 0x30, 0x80, 0),
                                GS_SETREG_RGBAQ(0x30, 0xD0, 0x30, 0x80, 0), GS_SETREG_RGBAQ(0x40, 0x80, 0xFF, 0x80, 0)};
    if (sPerfBars)
    {
        float k = gs->Width / 640.0f;
        for (int i = 0; i < 4; i++)
            gsKit_prim_sprite(gs, 4 * k, (4 + i * 6) * k, (4 + MS(prof[i]) * 30.0f + 1) * k, (8 + i * 6) * k, 1, cols[i]);
        gsKit_prim_sprite(gs, (4 + 16.7f * 30) * k, 2 * k, (6 + 16.7f * 30) * k, 30 * k, 1, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    }
    if (sUiHook) sUiHook();
    if (sHires)
    {
        uint32_t tf = BeCycles();
        gsKit_hires_sync(gs);
        gsKit_hires_flip(gs);
        sStageIdx ^= 1;
        prof[3] = BeCycles() - tf;
        return;
    }
    uint32_t te = BeCycles();
    gsKit_queue_exec(gs);
    prof[3] = BeCycles() - te;
    uint32_t tf = BeCycles();
    gsKit_sync_flip(gs);
    prof[3] += BeCycles() - tf; /* espera do vsync (nao e trabalho) */
}


/* ---- NES (R3): desenha o quadro de 256x240 (CT16) no modo de video atual. Reaproveita a VRAM da camada 0 (o GBA nao desenha enquanto o NES roda). ---- */
void BeNesDone(void)
{
    sForceUpload = sHires ? 6 : 2;               /* a VRAM das camadas foi usada pelo NES: reenviar tudo ao voltar */
    for (int i = 0; i < 4; i++) gBgLayer[i].dirty = 1;
    sClMode = -1;
}
void BeNesPresent(const uint16_t *pix, int overscan, int aspect, int linear, int scan, void (*overlay)(void))
{
    static GSTEXTURE nt;
    float fw = (float)gs->Width, fh = (float)gs->Height;
    sStageOff = 0;
    if (sHires) { gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0); gsKit_prim_sprite(gs, 0, 0, fw, fh, 1, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0)); }
    else gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    /* fracao da largura ocupada pela imagem: 480i/160p = TV 4:3; 480p/1080i = 16:9 (aspect 0 = 4:3, 1 = esticado) */
    float frac = (sMode >= 2 && aspect == 0) ? 0.75f : 1.0f;
    float w = fw * frac, x0 = (fw - w) * 0.5f, x1 = x0 + w, y0 = 0, y1 = fh;
    if (sMode == 3) { x0 -= 0.5f; x1 -= 0.5f; y0 -= 0.25f; y1 -= 0.25f; }
    memset(&nt, 0, sizeof nt);
    nt.Width = 256; nt.Height = 256; nt.PSM = GS_PSM_CT16; nt.TBW = 4;
    nt.Mem = (u32 *)pix; nt.Vram = tex[0].Vram;
    nt.Filter = linear ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    FlushCache(0);
    Send((void *)pix, 256, 240, nt.Vram, GS_PSM_CT16, 4, GS_CLUT_NONE);
    float v0 = overscan ? 8.0f : 0.0f, v1 = overscan ? 232.0f : 240.0f;
    gsKit_set_clamp(gs, GS_CMODE_CLAMP);
    gsKit_prim_sprite_texture(gs, &nt, x0, y0, 0.0f, v0, x1, y1, 256.0f, v1, 1, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    if (scan)
    {
        int lines = overscan ? 224 : 240;
        float sy = (y1 - y0) / lines;
        u64 k = GS_SETREG_RGBAQ(0, 0, 0, 0x48, 0);
        for (int r = 0; r < lines; r++)
        {
            float a = floorf(y0 + (r + 0.55f) * sy + 0.5f), b = floorf(y0 + (r + 1) * sy + 0.5f);
            if (b > a) gsKit_prim_sprite(gs, x0, a, x1, b, 1, k);
        }
    }
    if (overlay)
    {
        float sux = uiX, suw = uiW, suy = uiY, suh = uiH;
        uiX = 0; uiY = 0; uiW = fw; uiH = fh;
        if (sMode >= 2) { uiW = fw * 0.75f; uiX = (fw - uiW) * 0.5f; }
        overlay();
        uiX = sux; uiW = suw; uiY = suy; uiH = suh;
    }
    if (sHires) { gsKit_hires_sync(gs); gsKit_hires_flip(gs); sStageIdx ^= 1; return; }
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}
