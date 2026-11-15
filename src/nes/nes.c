#include <string.h>
#include <math.h>
#include "nes.h"
#include "cpu_tab.h"
#ifdef NES_PERF
unsigned gNesPerf[4];
static inline unsigned Cy(void) { unsigned c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
#define PERF_BEGIN unsigned _t0 = Cy();
#define PERF_END(i) gNesPerf[i] += Cy() - _t0;
#else
#define PERF_BEGIN
#define PERF_END(i)
#endif

/* ===================== estado ===================== */
static const uint8_t *sPrgRom; static uint32_t sPrgBanks;
static const uint8_t *sPrg[2];
static uint8_t sRam[0x800], sPrgRam[0x2000], sChr[0x2000], sVram[0x800];

/* CPU */
static uint8_t rA, rX, rY, rS, fC, fZ, fI, fD, fV, fN;
static uint16_t rPC;
static uint32_t sCyc;            /* ciclos de CPU desde o inicio */
static int sNmiPending, sIrqLine;
static unsigned sBadOps, sLastBad;
static uint8_t sOpen;
static uint32_t sWrCount, sIoCount, sRunTo;       /* escritas e leituras de I/O: um laco sem nenhuma delas e ocioso */
static uint16_t sIdleT; static uint8_t sIdleA, sIdleX, sIdleY, sIdleS, sIdleF; static uint32_t sIdleW, sIdleIo; static int sIdleArmed;

/* PPU */
static uint8_t sCtrl, sMask, sOam[256], sOamAddr, sPal[32], sRdBuf;
static uint16_t sV, sT; static uint8_t sFineX, sW;
static int sVbl, sSp0, sOvf, sLine;
static int sSp0Armed; static uint32_t sSp0Cyc;
static uint32_t sLineStart;

/* mapper MMC1 */
static uint8_t sMmcShift, sMmcCount, sMmcReg[4];

/* controle */
static uint8_t sPadNow, sPadShift, sStrobe;

static void MapperPrg(void)
{
    uint8_t c = sMmcReg[0], bank = sMmcReg[3] & 0x0F;
    int mode = (c >> 2) & 3;
    uint32_t nb = sPrgBanks;
    if (mode < 2) { uint32_t b = (bank & ~1u) % nb; sPrg[0] = sPrgRom + b * 0x4000; sPrg[1] = sPrgRom + ((b + 1) % nb) * 0x4000; }
    else if (mode == 2) { sPrg[0] = sPrgRom; sPrg[1] = sPrgRom + (bank % nb) * 0x4000; }
    else { sPrg[0] = sPrgRom + (bank % nb) * 0x4000; sPrg[1] = sPrgRom + (nb - 1) * 0x4000; }
}
/* ===================== APU ===================== */
static const uint8_t kLen[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t kDuty[4] = { 0x40, 0x60, 0x78, 0x9F };      /* bit 7 = 1o passo */
static const uint16_t kNoiseP[16] = { 4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
typedef struct { uint8_t en, duty, halt, constv, vol, estart, ediv, edecay, len, seq, swEn, swPer, swNeg, swShift, swDiv, swReload, ones; uint16_t timer; int32_t cnt; } Pulse;
static Pulse sP[2];
static struct { uint8_t en, ctrl, linRel, lin, linReload, len, seq; uint16_t timer; int32_t cnt; uint8_t out; } sTri;
static struct { uint8_t en, halt, constv, vol, estart, ediv, edecay, len, mode; uint16_t per; uint16_t shift; int32_t cnt; } sNoi;
static uint8_t sFcMode, sFcInhibit, sFcIrq; static int sFcStep; static int32_t sFcCnt;
static uint32_t sApuLast; static float sSampleLeft;
static float sSumP1, sSumP2, sSumT, sSumN; static int sWin;
static int16_t sAudio[4096]; static int sAudioN;
static float sHpX, sHpY;

static void EnvClock(uint8_t *start, uint8_t *div, uint8_t *decay, uint8_t vol, uint8_t halt)
{
    if (*start) { *start = 0; *decay = 15; *div = vol; }
    else if (*div == 0) { *div = vol; if (*decay) (*decay)--; else if (halt) *decay = 15; }
    else (*div)--;
}
static int PulseTarget(Pulse *p)
{
    int d = p->timer >> p->swShift;
    if (p->swNeg) d = -d - (p->ones ? 1 : 0);
    return p->timer + d;
}
static int PulseLevel(Pulse *p)
{
    if (!p->en || !p->len || p->timer < 8) return 0;
    if (!(p->swNeg) && PulseTarget(p) > 0x7FF) return 0;
    if (!((kDuty[p->duty] << p->seq) & 0x80)) return 0;
    return p->constv ? p->vol : p->edecay;
}
static void SweepClock(Pulse *p)
{
    int t = PulseTarget(p);
    if (p->swDiv == 0 && p->swEn && p->swShift && p->timer >= 8 && t <= 0x7FF && t >= 0) p->timer = (uint16_t)t;
    if (p->swDiv == 0 || p->swReload) { p->swDiv = p->swPer; p->swReload = 0; } else p->swDiv--;
}
static void QuarterFrame(void)
{
    for (int i = 0; i < 2; i++) EnvClock(&sP[i].estart, &sP[i].ediv, &sP[i].edecay, sP[i].vol, sP[i].halt);
    EnvClock(&sNoi.estart, &sNoi.ediv, &sNoi.edecay, sNoi.vol, sNoi.halt);
    if (sTri.linReload) sTri.lin = sTri.linRel; else if (sTri.lin) sTri.lin--;
    if (!sTri.ctrl) sTri.linReload = 0;
}
static void HalfFrame(void)
{
    for (int i = 0; i < 2; i++) { if (!sP[i].halt && sP[i].len) sP[i].len--; SweepClock(&sP[i]); }
    if (!sTri.ctrl && sTri.len) sTri.len--;
    if (!sNoi.halt && sNoi.len) sNoi.len--;
}
static const int kFc4[4] = { 7457, 7456, 7458, 7458 }, kFc5[5] = { 7457, 7456, 7458, 7458, 7452 };
static void FrameEvent(void)
{
    if (sFcMode == 0)
    {
        QuarterFrame(); if (sFcStep == 1 || sFcStep == 3) HalfFrame();
        if (sFcStep == 3 && !sFcInhibit) sFcIrq = 1;
        sFcStep = (sFcStep + 1) & 3; sFcCnt = kFc4[sFcStep];
    }
    else
    {
        if (sFcStep != 3) { QuarterFrame(); if (sFcStep == 1 || sFcStep == 4) HalfFrame(); }
        else {}
        sFcStep = (sFcStep + 1) % 5; sFcCnt = kFc5[sFcStep];
    }
}
static float sPulseTab[33], sTndTab[206];
static void MixInit(void)
{
    for (int i = 0; i < 33; i++) sPulseTab[i] = i ? 95.88f / (8128.0f / i + 100.0f) : 0.0f;
    for (int i = 0; i < 206; i++) sTndTab[i] = i ? 159.79f / (1.0f / (i / 24681.0f) + 100.0f) : 0.0f;   /* i = 3*triangulo + 2*ruido */
}
static inline float Lerp(const float *tab, float x, int max)
{
    if (x > (float)max) x = (float)max;
    int i = (int)x; float f = x - (float)i;
    return tab[i] + (tab[i + 1] - tab[i]) * f;
}
static inline float Mix(float p1, float p2, float t, float n)
{
    return Lerp(sPulseTab, p1 + p2, 30) + Lerp(sTndTab, 3.0f * t + 2.0f * n, 202);
}
static void ApuAdvance(int n)
{
    /* pulsos */
    for (int i = 0; i < 2; i++)
    {
        Pulse *p = &sP[i]; float *sum = i ? &sSumP2 : &sSumP1; int r = n;
        if (!p->en || !p->len) continue;   /* silencioso: nada a somar */
        int lvl = PulseLevel(p);
        while (r > 0)
        {
            int s = r < p->cnt ? r : p->cnt;
            *sum += (float)lvl * s; r -= s; p->cnt -= s;
            if (p->cnt <= 0) { p->cnt = (p->timer + 1) * 2; p->seq = (p->seq - 1) & 7; lvl = PulseLevel(p); }
        }
    }
    /* triangulo */
    if (!sTri.len || !sTri.lin) sSumT += (float)sTri.out * n;   /* parado: segura o ultimo nivel */
    else { int r = n;
      while (r > 0)
      {
          int s = r < sTri.cnt ? r : sTri.cnt;
          sSumT += (float)sTri.out * s; r -= s; sTri.cnt -= s;
          if (sTri.cnt <= 0)
          {
              sTri.cnt = sTri.timer + 1;
              if (sTri.len && sTri.lin && sTri.timer >= 2) { sTri.seq = (sTri.seq + 1) & 31; sTri.out = sTri.seq < 16 ? 15 - sTri.seq : sTri.seq - 16; }
          }
      } }
    /* ruido */
    if (sNoi.en && sNoi.len) { int r = n; int per = kNoiseP[sNoi.per];
      while (r > 0)
      {
          int s = r < sNoi.cnt ? r : sNoi.cnt;
          int lvl = (sNoi.en && sNoi.len && !(sNoi.shift & 1)) ? (sNoi.constv ? sNoi.vol : sNoi.edecay) : 0;
          sSumN += (float)lvl * s; r -= s; sNoi.cnt -= s;
          if (sNoi.cnt <= 0)
          {
              sNoi.cnt = per;
              int fb = (sNoi.shift & 1) ^ ((sNoi.shift >> (sNoi.mode ? 6 : 1)) & 1);
              sNoi.shift = (uint16_t)((sNoi.shift >> 1) | (fb << 14));
          }
      } }
    sWin += n;
}
static void ApuFlush(void)
{
    PERF_BEGIN
    int n = (int)(sCyc - sApuLast); sApuLast = sCyc;
    while (n > 0)
    {
        int s = n;
        if (sFcCnt < s) s = sFcCnt;
        int ts = (int)sSampleLeft + ((float)(int)sSampleLeft < sSampleLeft ? 1 : 0); if (ts < 1) ts = 1; if (ts < s) s = ts;
        ApuAdvance(s);
        sFcCnt -= s; if (sFcCnt <= 0) FrameEvent();
        sSampleLeft -= s;
        if (sSampleLeft <= 0)
        {
            sSampleLeft += 1789773.0f / NES_AUDIO_RATE;
            float w = sWin ? 1.0f / sWin : 0;
            float x = Mix(sSumP1 * w, sSumP2 * w, sSumT * w, sSumN * w);
            sSumP1 = sSumP2 = sSumT = sSumN = 0; sWin = 0;
            float y = x - sHpX + 0.9955f * sHpY; sHpX = x; sHpY = y;      /* tira o nivel DC */
            float o = y * 48000.0f; if (o > 32767) o = 32767; if (o < -32768) o = -32768;
            if (sAudioN < 4096) sAudio[sAudioN++] = (int16_t)o;
        }
        n -= s;
    }
    PERF_END(1)
}
int NesAudioTake(int16_t *dst, int max)
{
    int n = sAudioN < max ? sAudioN : max;
    memcpy(dst, sAudio, n * sizeof(int16_t));
    if (n < sAudioN) memmove(sAudio, sAudio + n, (sAudioN - n) * sizeof(int16_t));
    sAudioN -= n; return n;
}
static void ApuWrite(uint16_t a, uint8_t v)
{
    ApuFlush();
    if (a < 0x4008)
    {
        Pulse *p = &sP[(a >> 2) & 1];
        switch (a & 3)
        {
            case 0: p->duty = v >> 6; p->halt = (v >> 5) & 1; p->constv = (v >> 4) & 1; p->vol = v & 15; break;
            case 1: p->swEn = v >> 7; p->swPer = (v >> 4) & 7; p->swNeg = (v >> 3) & 1; p->swShift = v & 7; p->swReload = 1; break;
            case 2: p->timer = (p->timer & 0x700) | v; break;
            case 3: p->timer = (p->timer & 0xFF) | ((v & 7) << 8); if (p->en) p->len = kLen[v >> 3]; p->seq = 0; p->estart = 1; break;
        }
        return;
    }
    switch (a)
    {
        case 0x4008: sTri.ctrl = v >> 7; sTri.linRel = v & 0x7F; break;
        case 0x400A: sTri.timer = (sTri.timer & 0x700) | v; break;
        case 0x400B: sTri.timer = (sTri.timer & 0xFF) | ((v & 7) << 8); if (sTri.en) sTri.len = kLen[v >> 3]; sTri.linReload = 1; break;
        case 0x400C: sNoi.halt = (v >> 5) & 1; sNoi.constv = (v >> 4) & 1; sNoi.vol = v & 15; break;
        case 0x400E: sNoi.mode = v >> 7; sNoi.per = v & 15; break;
        case 0x400F: if (sNoi.en) sNoi.len = kLen[v >> 3]; sNoi.estart = 1; break;
        case 0x4015:
            sP[0].en = v & 1; sP[1].en = (v >> 1) & 1; sTri.en = (v >> 2) & 1; sNoi.en = (v >> 3) & 1;
            if (!sP[0].en) sP[0].len = 0; if (!sP[1].en) sP[1].len = 0; if (!sTri.en) sTri.len = 0; if (!sNoi.en) sNoi.len = 0;
            break;
        case 0x4017:
            sFcMode = v >> 7; sFcInhibit = (v >> 6) & 1; if (sFcInhibit) sFcIrq = 0;
            sFcStep = 0; sFcCnt = sFcMode ? kFc5[0] : kFc4[0];
            if (sFcMode) { QuarterFrame(); HalfFrame(); }
            break;
    }
}

/* ===================== PPU ===================== */
static int sMirror;      /* 0 = tela unica baixa, 1 = tela unica alta, 2 = vertical, 3 = horizontal */
static uint8_t *sNt[4];
static void MapNt(void)
{
    static const uint8_t m[4][4] = { {0,0,0,0}, {1,1,1,1}, {0,1,0,1}, {0,0,1,1} };
    for (int i = 0; i < 4; i++) sNt[i] = sVram + m[sMirror][i] * 0x400;
}

static inline uint8_t *NtPtr(uint16_t a) { return sNt[(a >> 10) & 3] + (a & 0x3FF); }
static inline uint8_t PpuRead(uint16_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000) return sChr[a];
    if (a < 0x3F00) return *NtPtr(a);
    int i = a & 0x1F; if ((i & 0x13) == 0x10) i &= ~0x10;
    return sPal[i];
}
static inline void PpuWrite(uint16_t a, uint8_t v)
{
    a &= 0x3FFF;
    if (a < 0x2000) sChr[a] = v;
    else if (a < 0x3F00) *NtPtr(a) = v;
    else { int i = a & 0x1F; if ((i & 0x13) == 0x10) i &= ~0x10; sPal[i] = v & 0x3F; }
}
static inline int Rendering(void) { return (sMask & 0x18) != 0; }

/* byte -> 8 nibbles (bit j vira o nibble j): decodifica uma linha de 8 pixels de um tile (plano 0 | plano 1 << 1) */
static uint32_t kSpread[256];
static uint16_t sPal16[64];
uint16_t gNesPix[NES_H_PX * NES_W] __attribute__((aligned(64)));
static void PpuLine(int ln)
{
    uint16_t *fb = gNesPix + ln * NES_W;
    int bgOn = (sMask & 8) != 0, spOn = (sMask & 0x10) != 0, gray = (sMask & 1) ? 0x30 : 0x3F;
    uint16_t back = sPal16[sPal[0] & gray];
    uint8_t opq[34];
    if (bgOn)
    {
        uint16_t line[272];
        uint16_t col[4][4];
        for (int p = 0; p < 4; p++) { col[p][0] = back; for (int k = 1; k < 4; k++) col[p][k] = sPal16[sPal[(p << 2) | k] & gray]; }
        int cx0 = sV & 31, cy = (sV >> 5) & 31, nt0 = (sV >> 10) & 3, fy = (sV >> 12) & 7;
        const uint8_t *chr = sChr + ((sCtrl & 0x10) ? 0x1000 : 0) + fy;
        const uint8_t *rowA = sNt[nt0] + (cy << 5), *rowB = sNt[nt0 ^ 1] + (cy << 5);
        const uint8_t *atA = sNt[nt0] + 0x3C0 + ((cy >> 2) << 3), *atB = sNt[nt0 ^ 1] + 0x3C0 + ((cy >> 2) << 3);
        int ash = (cy & 2) << 1;
        for (int i = 0; i < 33; i++)
        {
            int cx = cx0 + i; const uint8_t *row = rowA, *at = atA;
            if (cx >= 32) { cx &= 31; row = rowB; at = atB; }
            const uint8_t *t = chr + row[cx] * 16;
            uint8_t lo = t[0], hi = t[8];
            uint16_t *d = line + i * 8;
            opq[i] = (uint8_t)(lo | hi);
            if (!(lo | hi)) { d[0] = d[1] = d[2] = d[3] = d[4] = d[5] = d[6] = d[7] = back; continue; }
            const uint16_t *c = col[(at[cx >> 2] >> (ash | (cx & 2))) & 3];
            uint32_t a = kSpread[lo] | (kSpread[hi] << 1);
            d[0] = c[(a >> 28) & 3]; d[1] = c[(a >> 24) & 3]; d[2] = c[(a >> 20) & 3]; d[3] = c[(a >> 16) & 3];
            d[4] = c[(a >> 12) & 3]; d[5] = c[(a >> 8) & 3]; d[6] = c[(a >> 4) & 3]; d[7] = c[a & 3];
        }
        memcpy(fb, line + sFineX, NES_W * sizeof(uint16_t));
        if (!(sMask & 2)) for (int x = 0; x < 8; x++) fb[x] = back;
    }
    else { for (int x = 0; x < NES_W; x++) fb[x] = back; }
    if (spOn)
    {
        int h = (sCtrl & 0x20) ? 16 : 8, found = 0, sel[8];
        for (int s = 0; s < 64; s++)
        {
            unsigned row = (unsigned)(ln - (sOam[s * 4] + 1));
            if (row >= (unsigned)h) continue;
            if (found == 8) { sOvf = 1; break; }
            sel[found++] = s;
        }
        if (!found) return;
        uint8_t sprpix[264], sprinfo[264]; memset(sprpix, 0, sizeof sprpix);
        int minx = 256, maxx = -1;
        for (int j = found - 1; j >= 0; j--)
        {
            int s = sel[j]; const uint8_t *o = &sOam[s * 4];
            int row = ln - (o[0] + 1), tile = o[1], at = o[2], x0 = o[3];
            if (at & 0x80) row = h - 1 - row;
            uint16_t base;
            if (h == 16) { base = (tile & 1) ? 0x1000 : 0; tile &= 0xFE; if (row >= 8) { tile++; row -= 8; } }
            else base = (sCtrl & 8) ? 0x1000 : 0;
            uint8_t lo = sChr[base + tile * 16 + row], hi = sChr[base + tile * 16 + row + 8];
            if (!(lo | hi)) continue;
            uint32_t a = kSpread[lo] | (kSpread[hi] << 1);
            uint8_t pri = (uint8_t)((at & 0x20) | (s == 0 ? 1 : 0)), palb = (uint8_t)(0x10 | ((at & 3) << 2));
            if (x0 < minx) minx = x0;
            if (x0 + 7 > maxx) maxx = x0 + 7;
            for (int k = 0; k < 8; k++)
            {
                int x = x0 + k; if (x > 255) break;
                int sh = (at & 0x40) ? 4 * k : 28 - 4 * k;
                int px = (a >> sh) & 3;
                if (!px) continue;
                if (x < 8 && !(sMask & 4)) continue;
                sprpix[x] = (uint8_t)(palb | px);
                sprinfo[x] = pri;
            }
        }
        if (maxx > 255) maxx = 255;
        for (int x = minx; x <= maxx; x++)
        {
            if (!sprpix[x]) continue;
            int bgo = bgOn && x + sFineX < 264 && ((opq[(x + sFineX) >> 3] >> (7 - ((x + sFineX) & 7))) & 1) && !(x < 8 && !(sMask & 2));
            if ((sprinfo[x] & 1) && bgo && x < 255 && !sSp0 && !sSp0Armed)
            { sSp0Armed = 1; sSp0Cyc = sLineStart + (uint32_t)((x + 1) / 3); }
            if (!bgo || !(sprinfo[x] & 0x20)) fb[x] = sPal16[sPal[sprpix[x] & 0x1F] & gray];
        }
    }
}
static void PpuLineTimed(int ln) { PERF_BEGIN PpuLine(ln); PERF_END(0) }
static void IncY(void)
{
    if ((sV & 0x7000) != 0x7000) sV += 0x1000;
    else
    {
        sV &= ~0x7000; int y = (sV & 0x3E0) >> 5;
        if (y == 29) { y = 0; sV ^= 0x800; } else if (y == 31) y = 0; else y++;
        sV = (uint16_t)((sV & ~0x3E0) | (y << 5));
    }
}

static uint8_t PpuReg(uint16_t a, int wr, uint8_t v)
{
    uint8_t r = sOpen;
    switch (a & 7)
    {
        case 0: if (wr) { int wasNmi = sCtrl & 0x80; sCtrl = v; sT = (uint16_t)((sT & ~0x0C00) | ((v & 3) << 10)); if (!wasNmi && (v & 0x80) && sVbl) sNmiPending = 1; } break;
        case 1: if (wr) sMask = v; break;
        case 2:
            if (!wr)
            {
                if (sSp0Armed && sCyc >= sSp0Cyc) { sSp0 = 1; sSp0Armed = 0; }
                r = (uint8_t)((sVbl << 7) | (sSp0 << 6) | (sOvf << 5) | (sOpen & 0x1F)); sVbl = 0; sW = 0;
            }
            break;
        case 3: if (wr) sOamAddr = v; break;
        case 4: if (wr) { sOam[sOamAddr++] = v; } else r = sOam[sOamAddr]; break;
        case 5:
            if (wr)
            {
                if (!sW) { sT = (uint16_t)((sT & ~0x1F) | (v >> 3)); sFineX = v & 7; }
                else sT = (uint16_t)((sT & ~0x73E0) | ((v & 7) << 12) | ((v & 0xF8) << 2));
                sW ^= 1;
            }
            break;
        case 6:
            if (wr)
            {
                if (!sW) sT = (uint16_t)((sT & 0x00FF) | ((v & 0x3F) << 8));
                else { sT = (sT & 0xFF00) | v; sV = sT; }
                sW ^= 1;
            }
            break;
        case 7:
            if (wr) { PpuWrite(sV, v); sV += (sCtrl & 4) ? 32 : 1; }
            else
            {
                uint16_t ad = sV & 0x3FFF;
                if (ad >= 0x3F00) { r = PpuRead(ad); sRdBuf = PpuRead(ad - 0x1000); }
                else { r = sRdBuf; sRdBuf = PpuRead(ad); }
                sV += (sCtrl & 4) ? 32 : 1;
            }
            break;
    }
    if (wr) sOpen = v; else if ((a & 7) != 2) sOpen = r;
    return r;
}

/* ===================== barramento da CPU ===================== */
static uint8_t RdSlow(uint16_t a)
{
    sIoCount++;
    if (a < 0x4000) return PpuReg(a, 0, 0);
    if (a == 0x4015)
    {
        ApuFlush();
        uint8_t r = (uint8_t)((sP[0].len ? 1 : 0) | (sP[1].len ? 2 : 0) | (sTri.len ? 4 : 0) | (sNoi.len ? 8 : 0) | (sFcIrq ? 0x40 : 0));
        sFcIrq = 0; return r;
    }
    if (a == 0x4016) { uint8_t b = sPadShift & 1; sPadShift = (uint8_t)((sPadShift >> 1) | 0x80); return 0x40 | b; }
    if (a == 0x4017) return 0x40;
    if (a >= 0x6000 && a < 0x8000) return sPrgRam[a & 0x1FFF];
    return sOpen;
}
static inline uint8_t Rd(uint16_t a)
{
    if (a < 0x2000) return sRam[a & 0x7FF];
    if (a >= 0x8000) return sPrg[(a >> 14) & 1][a & 0x3FFF];
    return RdSlow(a);
}
static void Wr(uint16_t a, uint8_t v)
{
    sWrCount++;
    if (a < 0x2000) { sRam[a & 0x7FF] = v; return; }
    if (a < 0x4000) { PpuReg(a, 1, v); return; }
    if (a < 0x4018)
    {
        if (a == 0x4014)
        {
            uint16_t b = (uint16_t)(v << 8);
            for (int i = 0; i < 256; i++) sOam[(sOamAddr + i) & 255] = Rd((uint16_t)(b + i));
            sCyc += 513 + (sCyc & 1);
        }
        else if (a == 0x4016) { sStrobe = v & 1; if (sStrobe) sPadShift = sPadNow; else if (sStrobe == 0) { /* latch no fim do strobe */ sPadShift = sPadNow; } }
        else ApuWrite(a, v);
        return;
    }
    if (a >= 0x6000 && a < 0x8000) { sPrgRam[a & 0x1FFF] = v; return; }
    if (a >= 0x8000)
    {
        if (v & 0x80) { sMmcShift = 0; sMmcCount = 0; sMmcReg[0] |= 0x0C; }
        else
        {
            sMmcShift = (uint8_t)((sMmcShift >> 1) | ((v & 1) << 4));
            if (++sMmcCount == 5) { sMmcReg[(a >> 13) & 3] = sMmcShift; sMmcShift = 0; sMmcCount = 0; }
        }
        sMirror = sMmcReg[0] & 3; MapNt();
        MapperPrg();
    }
}

/* ===================== CPU ===================== */
#define PUSH(v) (sRam[0x100 | rS--] = (uint8_t)(v))
#define PULL() (sRam[0x100 | ++rS])
static inline uint8_t GetP(int b) { return (uint8_t)(fC | (fZ << 1) | (fI << 2) | (fD << 3) | (b ? 0x10 : 0) | 0x20 | (fV << 6) | (fN << 7)); }
static inline void SetP(uint8_t p) { fC = p & 1; fZ = (p >> 1) & 1; fI = (p >> 2) & 1; fD = (p >> 3) & 1; fV = (p >> 6) & 1; fN = p >> 7; }
#define NZ(v) (fZ = ((v) & 0xFF) == 0, fN = ((v) >> 7) & 1)

static void Interrupt(uint16_t vec, int brk)
{
    PUSH(rPC >> 8); PUSH(rPC & 0xFF); PUSH(GetP(brk)); fI = 1;
    rPC = (uint16_t)(Rd(vec) | (Rd(vec + 1) << 8)); sCyc += 7;
}
static inline void Adc(uint8_t m) { int r = rA + m + fC; fV = (~(rA ^ m) & (rA ^ r) & 0x80) != 0; fC = r > 255; rA = (uint8_t)r; NZ(rA); }
static inline void Cmp(uint8_t r, uint8_t m) { fC = r >= m; uint8_t d = (uint8_t)(r - m); NZ(d); }

/* Metroid passa ~80% do quadro num laco 'LDA flag / BNE / JMP' esperando o NMI. Se um laco repete com os mesmos registradores,
 * nenhuma escrita e nenhuma leitura de I/O, ele nao muda nada ate o proximo evento: pula os ciclos ate o fim da fatia. */
static inline void BackEdge(uint16_t target)
{
    uint8_t f = (uint8_t)(fC | (fZ << 1) | (fI << 2) | (fD << 3) | (fV << 4) | (fN << 5));
    if (sIdleArmed && sIdleT == target && sIdleA == rA && sIdleX == rX && sIdleY == rY && sIdleS == rS && sIdleF == f &&
        sIdleW == sWrCount && sIdleIo == sIoCount && !sNmiPending && !(sFcIrq && !fI))
    {
        if (sRunTo > sCyc) sCyc = sRunTo;
        sIdleArmed = 0;
        return;
    }
    sIdleArmed = 1; sIdleT = target; sIdleA = rA; sIdleX = rX; sIdleY = rY; sIdleS = rS; sIdleF = f; sIdleW = sWrCount; sIdleIo = sIoCount;
}
static void CpuStep(void)
{
    if (sNmiPending) { sNmiPending = 0; Interrupt(0xFFFA, 0); return; }
    if (sFcIrq && !fI) { Interrupt(0xFFFE, 0); return; }
    uint8_t op = Rd(rPC++);
    int am = kAm[op], o = kOp[op], cyc = kCyc[op];
    uint16_t ea = 0, base;
    int rd = 0;
    switch (am)
    {
        case AM_IMM: ea = rPC++; break;
        case AM_ZP: ea = Rd(rPC++); break;
        case AM_ZPX: ea = (uint8_t)(Rd(rPC++) + rX); break;
        case AM_ZPY: ea = (uint8_t)(Rd(rPC++) + rY); break;
        case AM_ABS: ea = (uint16_t)(Rd(rPC) | (Rd(rPC + 1) << 8)); rPC += 2; break;
        case AM_ABX: case AM_ABY:
            base = (uint16_t)(Rd(rPC) | (Rd(rPC + 1) << 8)); rPC += 2;
            ea = (uint16_t)(base + (am == AM_ABX ? rX : rY));
            rd = (o == OP_ADC || o == OP_AND || o == OP_CMP || o == OP_EOR || o == OP_LDA || o == OP_LDX || o == OP_LDY || o == OP_ORA || o == OP_SBC);
            if (rd && ((base ^ ea) & 0xFF00)) cyc++;
            break;
        case AM_IZX: { uint8_t z = (uint8_t)(Rd(rPC++) + rX); ea = (uint16_t)(Rd(z) | (Rd((uint8_t)(z + 1)) << 8)); } break;
        case AM_IZY: { uint8_t z = Rd(rPC++); base = (uint16_t)(Rd(z) | (Rd((uint8_t)(z + 1)) << 8)); ea = (uint16_t)(base + rY);
            rd = (o != OP_STA); if (rd && ((base ^ ea) & 0xFF00)) cyc++; } break;
        default: break;
    }
    switch (o)
    {
        case OP_LDA: rA = Rd(ea); NZ(rA); break;
        case OP_LDX: rX = Rd(ea); NZ(rX); break;
        case OP_LDY: rY = Rd(ea); NZ(rY); break;
        case OP_STA: Wr(ea, rA); break;
        case OP_STX: Wr(ea, rX); break;
        case OP_STY: Wr(ea, rY); break;
        case OP_TAX: rX = rA; NZ(rX); break;
        case OP_TAY: rY = rA; NZ(rY); break;
        case OP_TXA: rA = rX; NZ(rA); break;
        case OP_TYA: rA = rY; NZ(rA); break;
        case OP_TSX: rX = rS; NZ(rX); break;
        case OP_TXS: rS = rX; break;
        case OP_PHA: PUSH(rA); break;
        case OP_PHP: PUSH(GetP(1)); break;
        case OP_PLA: rA = PULL(); NZ(rA); break;
        case OP_PLP: SetP(PULL()); break;
        case OP_AND: rA &= Rd(ea); NZ(rA); break;
        case OP_ORA: rA |= Rd(ea); NZ(rA); break;
        case OP_EOR: rA ^= Rd(ea); NZ(rA); break;
        case OP_BIT: { uint8_t m = Rd(ea); fZ = (m & rA) == 0; fN = m >> 7; fV = (m >> 6) & 1; } break;
        case OP_ADC: Adc(Rd(ea)); break;
        case OP_SBC: Adc((uint8_t)~Rd(ea)); break;
        case OP_CMP: Cmp(rA, Rd(ea)); break;
        case OP_CPX: Cmp(rX, Rd(ea)); break;
        case OP_CPY: Cmp(rY, Rd(ea)); break;
        case OP_INC: { uint8_t m = (uint8_t)(Rd(ea) + 1); Wr(ea, m); NZ(m); } break;
        case OP_DEC: { uint8_t m = (uint8_t)(Rd(ea) - 1); Wr(ea, m); NZ(m); } break;
        case OP_INX: rX++; NZ(rX); break;
        case OP_INY: rY++; NZ(rY); break;
        case OP_DEX: rX--; NZ(rX); break;
        case OP_DEY: rY--; NZ(rY); break;
        case OP_ASL: if (am == AM_ACC) { fC = rA >> 7; rA <<= 1; NZ(rA); } else { uint8_t m = Rd(ea); fC = m >> 7; m <<= 1; Wr(ea, m); NZ(m); } break;
        case OP_LSR: if (am == AM_ACC) { fC = rA & 1; rA >>= 1; NZ(rA); } else { uint8_t m = Rd(ea); fC = m & 1; m >>= 1; Wr(ea, m); NZ(m); } break;
        case OP_ROL: if (am == AM_ACC) { uint8_t c = fC; fC = rA >> 7; rA = (uint8_t)((rA << 1) | c); NZ(rA); } else { uint8_t m = Rd(ea), c = fC; fC = m >> 7; m = (uint8_t)((m << 1) | c); Wr(ea, m); NZ(m); } break;
        case OP_ROR: if (am == AM_ACC) { uint8_t c = fC; fC = rA & 1; rA = (uint8_t)((rA >> 1) | (c << 7)); NZ(rA); } else { uint8_t m = Rd(ea), c = fC; fC = m & 1; m = (uint8_t)((m >> 1) | (c << 7)); Wr(ea, m); NZ(m); } break;
        case OP_JMP:
            if (am == AM_IND) { uint16_t p = (uint16_t)(Rd(rPC) | (Rd(rPC + 1) << 8)); rPC = (uint16_t)(Rd(p) | (Rd((p & 0xFF00) | ((p + 1) & 0xFF)) << 8)); }
            else { if (ea < rPC) BackEdge(ea); rPC = ea; }
            break;
        case OP_JSR: { uint16_t ret = (uint16_t)(rPC - 1); PUSH(ret >> 8); PUSH(ret & 0xFF); rPC = ea; } break;
        case OP_RTS: { uint8_t lo = PULL(), hi = PULL(); rPC = (uint16_t)((lo | (hi << 8)) + 1); } break;
        case OP_RTI: SetP(PULL()); { uint8_t lo = PULL(), hi = PULL(); rPC = (uint16_t)(lo | (hi << 8)); } break;
        case OP_BRK: rPC++; Interrupt(0xFFFE, 1); cyc = 0; break;
        case OP_CLC: fC = 0; break; case OP_SEC: fC = 1; break;
        case OP_CLI: fI = 0; break; case OP_SEI: fI = 1; break;
        case OP_CLD: fD = 0; break; case OP_SED: fD = 1; break;
        case OP_CLV: fV = 0; break;
        case OP_NOP: break;
        case OP_BPL: case OP_BMI: case OP_BVC: case OP_BVS: case OP_BCC: case OP_BCS: case OP_BNE: case OP_BEQ:
        {
            int8_t d = (int8_t)Rd(rPC++); int t;
            switch (o) { case OP_BPL: t = !fN; break; case OP_BMI: t = fN; break; case OP_BVC: t = !fV; break; case OP_BVS: t = fV; break;
                         case OP_BCC: t = !fC; break; case OP_BCS: t = fC; break; case OP_BNE: t = !fZ; break; default: t = fZ; break; }
            if (t) { uint16_t n = (uint16_t)(rPC + d); cyc += 1 + (((n ^ rPC) & 0xFF00) != 0); if (d < 0) BackEdge(n); rPC = n; }
        } break;
        default: sBadOps++; sLastBad = op; break;      /* opcode nao oficial: tratado como NOP de 1 byte */
    }
    sCyc += cyc;
}

/* ===================== quadro ===================== */
void NesInit(const uint8_t *prg, uint32_t size)
{
    sPrgRom = prg; sPrgBanks = size / 0x4000;
    memset(sRam, 0, sizeof sRam); memset(sChr, 0, sizeof sChr); memset(sVram, 0, sizeof sVram); memset(sPal, 0x0F, sizeof sPal);
    memset(sOam, 0xFF, sizeof sOam); memset(gNesPix, 0, sizeof gNesPix);
    for (int i = 0; i < 256; i++) { uint32_t v = 0; for (int j = 0; j < 8; j++) v |= (uint32_t)((i >> j) & 1) << (4 * j); kSpread[i] = v; }
    for (int i = 0; i < 64; i++) { uint32_t c = kNesPalette[i]; sPal16[i] = (uint16_t)(0x8000 | (((c >> 19) & 31) << 10) | (((c >> 11) & 31) << 5) | ((c >> 3) & 31)); }
    memset(sP, 0, sizeof sP); memset(&sTri, 0, sizeof sTri); memset(&sNoi, 0, sizeof sNoi);
    sNoi.shift = 1; sP[0].ones = 1; sTri.cnt = 1; sP[0].cnt = sP[1].cnt = 2; sNoi.cnt = 4;
    MixInit();
    sFcMode = 0; sFcStep = 0; sFcCnt = kFc4[0]; sFcIrq = 0; sFcInhibit = 0;
    sApuLast = 0; sSampleLeft = 1789773.0f / NES_AUDIO_RATE; sAudioN = 0; sHpX = sHpY = 0;
    memset(sMmcReg, 0, sizeof sMmcReg); sMmcReg[0] = 0x0C; sMmcShift = sMmcCount = 0; sMirror = 0; MapNt(); MapperPrg();
    rA = rX = rY = 0; rS = 0xFD; fI = 1; fC = fZ = fD = fV = fN = 0; sCyc = 0; sNmiPending = 0;
    sCtrl = sMask = 0; sV = sT = 0; sFineX = 0; sW = 0; sVbl = sSp0 = sOvf = 0; sSp0Armed = 0; sLine = 0;
    rPC = (uint16_t)(Rd(0xFFFC) | (Rd(0xFFFD) << 8));
    sCyc = 7; sApuLast = 7;
}

void NesFrame(uint8_t pad)
{
    sPadNow = pad;
    static int frac;
    static uint32_t lineEnd;
    if (!lineEnd) lineEnd = sCyc;
    for (int ln = 0; ln < 262; ln++)
    {
        sLine = ln; sLineStart = lineEnd;
        if (ln == 241) { sVbl = 1; if (sCtrl & 0x80) sNmiPending = 1; }
        if (ln == 261) { sVbl = 0; sSp0 = 0; sSp0Armed = 0; sOvf = 0; }
        if (ln < 240) { if (Rendering()) PpuLineTimed(ln); else { uint16_t bk = sPal16[sPal[0] & 0x3F]; for (int x = 0; x < NES_W; x++) gNesPix[ln * NES_W + x] = bk; } }
        lineEnd += 113; frac += 2; if (frac >= 3) { frac -= 3; lineEnd++; }
        uint32_t mid = sLineStart + 86;
        sRunTo = mid; while (sCyc < mid) CpuStep();
        if (Rendering() && (ln < 240 || ln == 261))
        {
            if (ln < 240) IncY();
            sV = (uint16_t)((sV & ~0x041F) | (sT & 0x041F));
        }
        if (ln == 261) { uint32_t m2 = sLineStart + 96; sRunTo = m2; while (sCyc < m2) CpuStep(); if (Rendering()) sV = (uint16_t)((sV & ~0x7BE0) | (sT & 0x7BE0)); }
        sRunTo = lineEnd; while (sCyc < lineEnd) CpuStep();
    }
    ApuFlush();
}
void NesStats(unsigned *b, unsigned *l) { *b = sBadOps; *l = sLastBad; }

const uint32_t kNesPalette[64] = {
#define C(r,g,b) (((b) << 16) | ((g) << 8) | (r))
 C(84,84,84),C(0,30,116),C(8,16,144),C(48,0,136),C(68,0,100),C(92,0,48),C(84,4,0),C(60,24,0),C(32,42,0),C(8,58,0),C(0,64,0),C(0,60,0),C(0,50,60),C(0,0,0),C(0,0,0),C(0,0,0),
 C(152,150,152),C(8,76,196),C(48,50,236),C(92,30,228),C(136,20,176),C(160,20,100),C(152,34,32),C(120,60,0),C(84,90,0),C(40,114,0),C(8,124,0),C(0,118,40),C(0,102,120),C(0,0,0),C(0,0,0),C(0,0,0),
 C(236,238,236),C(76,154,236),C(120,124,236),C(176,98,236),C(228,84,236),C(236,88,180),C(236,106,100),C(212,136,32),C(160,170,0),C(116,196,0),C(76,208,32),C(56,204,108),C(56,180,204),C(60,60,60),C(0,0,0),C(0,0,0),
 C(236,238,236),C(168,204,236),C(188,188,236),C(212,178,236),C(236,174,236),C(236,174,212),C(236,180,176),C(228,196,144),C(204,210,120),C(180,222,120),C(168,226,144),C(152,226,180),C(160,214,228),C(160,162,160),C(0,0,0),C(0,0,0)
#undef C
};

/* ===================== ROM do Zero Mission ===================== */

static uint32_t Rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }
/* LZ77 da BIOS do GBA (tipo 0x10). Devolve o tamanho gerado ou 0 em erro. */
static uint32_t LzBios(const uint8_t *src, uint32_t avail, uint8_t *dst, uint32_t cap)
{
    if (avail < 4 || src[0] != 0x10) return 0;
    uint32_t size = (uint32_t)(src[1] | (src[2] << 8) | (src[3] << 16)), i = 4, o = 0;
    if (size > cap) return 0;
    while (o < size)
    {
        if (i >= avail) return 0;
        uint8_t fl = src[i++];
        for (int b = 0; b < 8 && o < size; b++, fl <<= 1)
        {
            if (!(fl & 0x80)) { if (i >= avail) return 0; dst[o++] = src[i++]; }
            else
            {
                if (i + 1 >= avail) return 0;
                uint32_t n = (src[i] >> 4) + 3, win = (uint32_t)(((src[i] & 15) << 8) | src[i + 1]) + 1; i += 2;
                if (win > o) return 0;
                while (n-- && o < size) { dst[o] = dst[o - win]; o++; }
            }
        }
    }
    return size;
}

/* O PRG do NES dentro do Zero Mission vem SEM os graficos: 25 blocos (LZ da BIOS) ficam numa tabela a parte e o
 * emulador original os recoloca na RAM. Aqui: descomprime a ROM (LZ proprio) e recoloca os blocos. */
int NesExtractMetroid(const uint8_t *gba, uint32_t size, uint8_t *out)
{
    const uint32_t PTR = 0x7D80CC, COMP = 0x16437, DEC = 0x20000, NGFX = 25;
    if (size < PTR + 8) return 1;
    uint32_t end = Rd32(gba + PTR) - 0x08000000u, tab = end, tabEnd = Rd32(gba + PTR + 4) - 0x08000000u;
    if (end < COMP || end > size || tabEnd <= tab || tabEnd > size) return 2;
    uint32_t i = (end - COMP) & 0xFFFFFC, dst = 0;
    int done = 0;
    while (!done)
    {
        if (i >= size) return 3;
        uint8_t flag = gba[i++];
        for (int b = 0; b < 8; b++)
        {
            if (flag & 0x80) { if (i >= size || dst >= DEC) return 3; out[dst++] = gba[i++]; }
            else
            {
                if (i + 1 >= size) return 3;
                uint32_t n = gba[i] >> 4, win = (uint32_t)(((gba[i] & 15) << 8) + gba[i + 1] + 1); i += 2;
                if (!n) n = 0x10 + gba[i++];
                n += 2;
                if (win > dst || dst + n > DEC) return 4;
                while (n--) { out[dst] = out[dst - win]; dst++; }
            }
            if (dst >= DEC) { done = 1; break; }
            flag <<= 1;
        }
    }
    /* graficos */
    const uint8_t *g = gba + tab; uint32_t gl = tabEnd - tab, src = NGFX * 4;
    for (uint32_t e = 0; e < NGFX; e++)
    {
        if (src >= gl) return 5;
        int16_t d = (int16_t)(g[e * 4] | (g[e * 4 + 1] << 8)), nx = (int16_t)(g[e * 4 + 2] | (g[e * 4 + 3] << 8));
        uint32_t off = (uint32_t)((int32_t)d << 4);
        static uint8_t tmp[0x1000];
        uint32_t n = LzBios(g + src, gl - src, tmp, sizeof tmp);
        if (!n || off + n > DEC) return 6;
        memcpy(out + off, tmp, n);
        src += (uint32_t)(int32_t)nx;
    }
    return 0;
}

