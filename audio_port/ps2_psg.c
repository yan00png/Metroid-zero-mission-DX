/* Sintetizador PSG do GBA para o PS2 (ver ps2_psg.h). */
#include "ps2_psg.h"
#include "port_gba_mem.h"
#include <stdint.h>

#define R_NR10 0x60
#define R_NR11 0x62 /* canal 1: duty/comprimento (byte baixo) + envelope (byte alto) */
#define R_NR13 0x64 /* canal 1: frequencia + controle */
#define R_NR21 0x68
#define R_NR23 0x6C
#define R_NR30 0x70
#define R_NR31 0x72 /* canal 3: comprimento (baixo) + volume (alto) */
#define R_NR33 0x74
#define R_NR41 0x78
#define R_NR43 0x7C /* canal 4: ruido (baixo) + controle (alto) */
#define R_CNT_L 0x80
#define R_CNT_H 0x82
#define R_CNT_X 0x84
#define R_WAVE 0x90

static unsigned Rd8(unsigned o) { return gIoMem[o]; }
static unsigned Rd16(unsigned o) { return gIoMem[o] | ((unsigned)gIoMem[o + 1] << 8); }

/* um canal de envelope (quadrados e ruido) */
struct Env
{
    int vol;            /* 0..15 */
    int dir;            /* +1 cresce, -1 decai */
    uint32_t period;    /* amostras de saida por passo; 0 = envelope parado */
    uint32_t count;
    unsigned lastByte;  /* ultimo byte de envelope visto */
    int trig;
};

struct Sq
{
    struct Env env;
    uint32_t phase, inc, duty;
    unsigned lastCtrl;
    int l, r;
};

static struct Sq sSq[2];
static struct Env sNoiEnv;
static uint32_t sNoiAcc, sNoiRate, sNoiLfsr = 0x7FFF;
static int sNoiW7, sNoiL, sNoiR, sNoiLevel = 1;
static unsigned sNoiLastCtrl;

static struct
{
    uint32_t phase, inc;
    int shift;      /* 0=100% 1=50% 2=25%, -1=75%, 9=mudo */
    int on, l, r, trig;
    unsigned lastCtrl;
} sWave;

static int sMaster, sScaleL, sScaleR, sActive;
static unsigned sRate = 48000;

void Ps2Psg_Reset(void)
{
    for (int i = 0; i < 2; i++)
    {
        sSq[i] = (struct Sq){0};
        sSq[i].env.dir = -1;
        sSq[i].lastCtrl = 0xFFFFFFFFu;
        sSq[i].env.lastByte = 0xFFFFFFFFu;
    }
    sNoiEnv = (struct Env){0};
    sNoiEnv.dir = -1;
    sNoiEnv.lastByte = 0xFFFFFFFFu;
    sNoiLfsr = 0x7FFF;
    sNoiAcc = 0;
    sNoiLastCtrl = 0xFFFFFFFFu;
    sWave.phase = sWave.inc = 0;
    sWave.shift = 9;
    sWave.on = 0;
    sWave.lastCtrl = 0xFFFFFFFFu;
    sMaster = sActive = 0;
}

void Ps2Psg_Trigger(unsigned ch)
{
    switch (ch)
    {
    case 1: sSq[0].env.trig = 1; break;
    case 2: sSq[1].env.trig = 1; break;
    case 3: sWave.trig = 1; break;
    case 4: sNoiEnv.trig = 1; break;
    }
}

/* o jogo roda o proprio envelope por software e reescreve o byte (passo 0) a cada mudanca de nivel: o volume
 * vale o do registrador sempre que o byte mudar ou houver nota nova; com passo != 0 o envelope evolui como no chip */
static void EnvLatch(struct Env *e, unsigned byte, int force)
{
    int step = byte & 7;
    e->dir = (byte & 8) ? 1 : -1;
    e->period = step ? (sRate * (uint32_t)step) / 64u : 0;
    if (force || byte != e->lastByte)
    {
        e->vol = (byte >> 4) & 15;
        e->count = e->period;
    }
    if (!step) e->vol = (byte >> 4) & 15;
    e->lastByte = byte;
    e->trig = 0;
}

static void EnvStep(struct Env *e)
{
    if (!e->period) return;
    if (--e->count) return;
    e->count = e->period;
    int v = e->vol + e->dir;
    if (v >= 0 && v <= 15) e->vol = v;
}

void Ps2Psg_Frame(unsigned outRate)
{
    if (outRate) sRate = outRate;
    sMaster = (Rd8(R_CNT_X) & 0x80) != 0;

    unsigned cl = Rd16(R_CNT_L);
    unsigned mvR = cl & 7, mvL = (cl >> 4) & 7;
    unsigned panR = (cl >> 8) & 15, panL = (cl >> 12) & 15;
    unsigned ratio = 4;                       /* PSG a 25/50/100% => 1/2/4 */
    switch (Rd16(R_CNT_H) & 3)
    {
    case 0: ratio = 1; break;
    case 1: ratio = 2; break;
    }
    sScaleL = (int)((mvL + 1) * ratio);
    sScaleR = (int)((mvR + 1) * ratio);

    static const unsigned kDuty[2] = {R_NR11, R_NR21}, kCtl[2] = {R_NR13, R_NR23};
    static const uint32_t kThresh[4] = {0x20000000u, 0x40000000u, 0x80000000u, 0xC0000000u};
    for (int i = 0; i < 2; i++)
    {
        struct Sq *q = &sSq[i];
        unsigned d = Rd16(kDuty[i]), c = Rd16(kCtl[i]);
        q->duty = kThresh[(d >> 6) & 3];
        q->l = (panL >> i) & 1;
        q->r = (panR >> i) & 1;
        unsigned f = c & 0x7FF;
        /* f = 131072/(2048-x) Hz; fase de 32 bits => inc = f/taxa * 2^32 */
        q->inc = (uint32_t)(((uint64_t)131072 << 32) / ((uint64_t)(2048 - f) * sRate));
        int trig = q->env.trig || (c != q->lastCtrl && (c & 0x8000));
        EnvLatch(&q->env, (d >> 8) & 0xFF, trig);
        if (trig) q->phase = 0;
        q->lastCtrl = c;
    }

    {
        unsigned n30 = Rd8(R_NR30), n31 = Rd16(R_NR31), c = Rd16(R_NR33);
        sWave.on = (n30 & 0x80) != 0;
        sWave.l = (panL >> 2) & 1;
        sWave.r = (panR >> 2) & 1;
        if (n31 & 0x8000)
            sWave.shift = -1;
        else
            switch ((n31 >> 13) & 3)
            {
            case 0: sWave.shift = 9; break;
            case 1: sWave.shift = 0; break;
            case 2: sWave.shift = 1; break;
            default: sWave.shift = 2; break;
            }
        unsigned f = c & 0x7FF;
        sWave.inc = (uint32_t)(((uint64_t)65536 << 32) / ((uint64_t)(2048 - f) * sRate));
        if (sWave.trig || (c != sWave.lastCtrl && (c & 0x8000))) sWave.phase = 0;
        sWave.trig = 0;
        sWave.lastCtrl = c;
    }

    {
        unsigned n41 = Rd16(R_NR41), c = Rd16(R_NR43);
        sNoiL = (panL >> 3) & 1;
        sNoiR = (panR >> 3) & 1;
        sNoiW7 = (c & 8) != 0;
        unsigned r = c & 7, s = (c >> 4) & 15;
        uint32_t hz = r ? (524288u / r) >> (s + 1) : 1048576u >> (s + 1);
        if (!hz) hz = 1;
        sNoiRate = (uint32_t)(((uint64_t)hz << 16) / sRate); /* passos do LFSR por amostra, 16.16 */
        int trig = sNoiEnv.trig || (c != sNoiLastCtrl && (c & 0x8000));
        EnvLatch(&sNoiEnv, (n41 >> 8) & 0xFF, trig);
        if (trig) sNoiLfsr = 0x7FFF;
        sNoiLastCtrl = c;
    }

    sActive = sMaster && (((sSq[0].l || sSq[0].r) && sSq[0].env.vol) || ((sSq[1].l || sSq[1].r) && sSq[1].env.vol) ||
                          (sWave.on && (sWave.l || sWave.r) && sWave.shift != 9) || ((sNoiL || sNoiR) && sNoiEnv.vol));
}

int Ps2Psg_Active(void) { return sActive; }

/* media do quadrado sobre o intervalo [phase, phase+inc): fracao do tempo em nivel alto, 0..256 */
static int SquareCoverage(struct Sq *q)
{
    uint64_t p0 = q->phase, p1 = p0 + q->inc;
    uint64_t T = q->duty;
    uint64_t c0 = p0 < T ? p0 : T;
    uint32_t r1 = (uint32_t)p1;
    uint64_t c1 = (p1 >> 32) * T + (r1 < T ? r1 : T);
    q->phase = r1;
    if (!q->inc) return 0;
    uint64_t span = c1 - c0;
    uint64_t v = (span << 8) / q->inc;
    return v > 256 ? 256 : (int)v;
}

void Ps2Psg_Render(int *outL, int *outR)
{
    int l = 0, r = 0;
    if (!sActive)
    {
        *outL = *outR = 0;
        return;
    }

    for (int i = 0; i < 2; i++)
    {
        struct Sq *q = &sSq[i];
        int cov = SquareCoverage(q);
        EnvStep(&q->env);
        if (!q->inc || !q->env.vol) continue;
        int a = (2 * cov - 256) * q->env.vol; /* +-256*15 */
        if (q->l) l += a;
        if (q->r) r += a;
    }

    if (sWave.on && sWave.shift != 9)
    {
        int sum = 0;
        for (int k = 0; k < 4; k++)
        {
            unsigned idx = (uint32_t)(sWave.phase + (sWave.inc >> 2) * k) >> 27;
            unsigned b = gIoMem[R_WAVE + (idx >> 1)];
            int nib = (idx & 1) ? (b & 15) : (b >> 4);
            int a2 = 2 * nib - 15; /* -15..15 */
            sum += sWave.shift < 0 ? (a2 * 3) >> 2 : a2 >> sWave.shift;
        }
        sWave.phase += sWave.inc;
        int a = sum * 64; /* media de 4 * 256 */
        if (sWave.l) l += a;
        if (sWave.r) r += a;
    }
    else
        sWave.phase += sWave.inc;

    {
        sNoiAcc += sNoiRate;
        unsigned steps = sNoiAcc >> 16;
        sNoiAcc &= 0xFFFF;
        EnvStep(&sNoiEnv);
        int acc = 0, cnt = 0;
        if (steps > 24) steps = 24;
        for (unsigned k = 0; k < steps; k++)
        {
            unsigned x = (sNoiLfsr ^ (sNoiLfsr >> 1)) & 1;
            sNoiLfsr = (sNoiLfsr >> 1) | (x << 14);
            if (sNoiW7) sNoiLfsr = (sNoiLfsr & ~(1u << 6)) | (x << 6);
            sNoiLevel = (sNoiLfsr & 1) ? -1 : 1;
            acc += sNoiLevel;
            cnt++;
        }
        int avg256 = cnt ? (acc * 256) / cnt : sNoiLevel * 256;
        if (sNoiEnv.vol)
        {
            int a = avg256 * sNoiEnv.vol;
            if (sNoiL) l += a;
            if (sNoiR) r += a;
        }
    }

    /* (2c-1)*vol*escala*8/256 por canal => /32 ; ja em escala s16 */
    *outL = (l * sScaleL) >> 5;
    *outR = (r * sScaleR) >> 5;
}
