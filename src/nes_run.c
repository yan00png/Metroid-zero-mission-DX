/* Opcao METROID do menu: roda o emulador de NES (src/nes) com o Metroid de NES que esta dentro da propria ROM do Zero Mission. */
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <kernel.h>
#include <stdio.h>
#include <audsrv.h>
#include "nes/nes.h"
#include "platform.h"
#include "gfx_backend.h"
#include "rom_ref.h"

extern uint8_t gRomData[];
extern int MenuNes(const uint16_t *pix);
extern int gNesOpt[4];
extern void MenuStatus(const char *);

static uint8_t sPrg[0x20000] __attribute__((aligned(64)));
static int sReady;   /* 1 = ROM extraida e NES iniciado; -1 = falhou */

/* mesmo esquema do audio do PS2 do port: reamostra (+-2%) para manter ~1 quadro no buffer do audsrv */
static void Audio(void)
{
    static int16_t in[2048]; static short out[2100 * 2] __attribute__((aligned(64)));
    static int fill = -1;
    int n = NesAudioTake(in, 2048);
    if (n < 2) return;
    int m = n, q = audsrv_queued();
    if (q >= 0)
    {
        int frames = q >> 2;
        fill = fill < 0 ? frames : (fill * 7 + frames) >> 3;
        int e = ((fill - 900) * 20) / 900; if (e > 20) e = 20; if (e < -20) e = -20;
        m = n * (1000 - e) / 1000;
    }
    const int vm = AudioGain(gAudioVol[0]);
    int step = m > 1 ? (int)((((uint32_t)(n - 1)) << 16) / (uint32_t)(m - 1)) : 0, pos = 0;
    for (int i = 0; i < m; i++, pos += step)
    {
        int k = pos >> 16, f = (pos >> 8) & 255, k1 = k + 1 < n ? k + 1 : k;
        int v = in[k] + (((in[k1] - in[k]) * f) >> 8);
        v = (v * vm) >> 8;
        out[i * 2] = out[i * 2 + 1] = (short)v;
    }
    int room = audsrv_available() & ~3, bytes = m * 4;
    if (bytes > room) bytes = room;
    if (bytes > 0) audsrv_play_audio((const char *)out, bytes);
}

#ifdef NES_PERF
#include <stdio.h>
extern unsigned gNesPerf[4];
static unsigned Cy(void) { unsigned c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
static void PLOG(const char *f, ...) { va_list a; va_start(a, f); FILE *o = fopen("host:/tmp/ps2/nes_perf.txt", "a"); if (o) { vfprintf(o, f, a); fputc('\n', o); fclose(o); } va_end(a); }
#endif
void NesRun(void)
{
    if (!sReady)
    {
        int xr = NesExtractMetroid(gRomData, MZM_ROM_SIZE, sPrg);
        if (xr) { sReady = -1; }
        else { NesInit(sPrg, sizeof sPrg); sReady = 1; }
    }
    if (sReady < 0) return;
    audsrv_stop_audio();
    uint16_t prev = 0xFFFF;   /* botoes apertados na entrada nao contam ate soltar */
    for (;;)
    {
        PlatformBeat();                                               /* o watchdog de travamento precisa do 'coracao' */
        uint16_t p = PlatformReadPad(), e = p & ~prev; prev = p;
        if ((p & 0x0C) == 0x0C && (e & 0x0C))                        /* START+SELECT: pausa */
        {
            audsrv_stop_audio();
            int r = MenuNes(gNesPix);
            if (r == 1) NesInit(sPrg, sizeof sPrg);
            if (r == 2) break;
            prev = 0xFFFF;
            continue;
        }
        uint8_t n = 0;
        if (p & (1 << 0)) n |= NES_PAD_A;       /* X */
        if (p & (1 << 1)) n |= NES_PAD_B;       /* Quadrado */
        if (p & (1 << 6)) n |= NES_PAD_UP;
        if (p & (1 << 7)) n |= NES_PAD_DOWN;
        if (p & (1 << 5)) n |= NES_PAD_LEFT;
        if (p & (1 << 4)) n |= NES_PAD_RIGHT;
        if ((p & 0x0C) != 0x0C) { if (p & (1 << 3)) n |= NES_PAD_START; if (p & (1 << 2)) n |= NES_PAD_SELECT; }
#ifdef NES_PERF
        unsigned t0 = Cy();
#endif
        NesFrame(n);
#ifdef NES_PERF
        unsigned t1 = Cy();
#endif
        Audio();
#ifdef NES_PERF
        unsigned t2 = Cy();
#endif
        BeNesPresent(gNesPix, gNesOpt[0], gNesOpt[1], gNesOpt[2], gNesOpt[3], 0);
#ifdef NES_PERF
        { static unsigned acc[4]; static int nf; acc[0] += t1 - t0; acc[1] += t2 - t1; acc[2] += Cy() - t2;
          if (++nf == 60) { PLOG("por quadro (ciclos EE, orcamento 4.9M): NesFrame=%u (PPU=%u APU=%u) Audio+Convert=%u Present(c/ espera vsync)=%u", acc[0] / 60, gNesPerf[0] / 60, gNesPerf[1] / 60, acc[1] / 60, acc[2] / 60);
                            nf = 0; acc[0] = acc[1] = acc[2] = 0; gNesPerf[0] = gNesPerf[1] = 0; } }
#endif
    }
    audsrv_stop_audio();
    BeNesDone();
}
