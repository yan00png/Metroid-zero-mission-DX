/* Saida de audio do PS2 (libsd + audsrv) e ponte ROM <-> motor de som.
 * O motor (audio_port/ps2_engine.c) entrega as FIFOs A e B (8 bits) na taxa do jogo (13379..42048 Hz);
 * aqui: reamostra (cubica) para 48 kHz, soma o PSG (ps2_psg.c, ja a 48 kHz), roteia L/R conforme SOUNDCNT_H e
 * ajusta levemente a taxa para manter o buffer do audsrv estavel (o jogo roda a ~59,94 Hz, o GBA a 59,73 Hz). */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>
#include <sbv_patches.h>
#include "rom_ref.h"
#include "port_gba_mem.h"
#include "ps2_psg.h"

extern unsigned char libsd_irx[], audsrv_irx[];
extern unsigned int size_libsd_irx, size_audsrv_irx;

/* ---- ROM residente: os dados de som apontam para enderecos 0x08xxxxxx da ROM do GBA ---- */
uint8_t gRomData[MZM_ROM_SIZE] __attribute__((aligned(64)));

void *port_resolve_addr(uintptr_t val)
{
    if (val >= 0x08000000u && val < 0x0E000000u)
    {
        uintptr_t off = val & 0x01FFFFFFu;
        if (off < MZM_ROM_SIZE) return gRomData + off;
    }
    return (void *)val;
}

/* tabela de sons: copiada da ROM (cada entrada = ponteiro bruto do cabecalho + 2 u16) */
#define SOUND_COUNT_PS2 708
#define SOUND_TABLE_ROM_OFFSET 0x8f2c0u
struct SoundEntryRaw { uint32_t pHeader; uint16_t a, b; };
struct SoundEntryRaw sSoundDataEntries[SOUND_COUNT_PS2];

void AudioPs2_OnRomLoaded(void)
{
    memcpy(sSoundDataEntries, gRomData + SOUND_TABLE_ROM_OFFSET, sizeof sSoundDataEntries);
}

/* ---- saida ---- */
#define OUT_RATE 48000
static int sAudioOk;

int AudioPs2_Init(void)
{
    int ret = 0;
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    int a = SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, &ret);
    int b = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &ret);
    if (a < 0 || b < 0) return 0;
    if (audsrv_init() != 0) return 0;
    audsrv_fmt_t fmt = { .freq = OUT_RATE, .bits = 16, .channels = 2 };
    if (audsrv_set_format(&fmt) != 0) return 0;
    audsrv_set_volume(MAX_VOLUME);
    Ps2Psg_Reset();
    sAudioOk = 1;
    return 1;
}

int AudioPs2_Enabled(void) { return sAudioOk; }

/* volumes do usuario (menu AUDIO): 0 geral, 1 DirectSound (musica/voz por amostras), 2 PSG (canais 1-4); 0..100 % */
int gAudioVol[3] = { 100, 100, 100 };
int gAudioMono;
int AudioGain(int pct)   /* 0..100 % -> ganho 0..256 com curva quadratica (mais natural ao ouvido que a linear) */
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct * pct * 256 / 10000;
}


/* Catmull-Rom em ponto fixo: p0..p3 em 1/256 de LSB de 8 bits (escala s16), t em 0..4095 */
static inline int Cubic(int p0, int p1, int p2, int p3, int t)
{
    int c1 = p2 - p0;
    int c2 = 2 * p0 - 5 * p1 + 4 * p2 - p3;
    int c3 = 3 * (p1 - p2) + p3 - p0;
    int v = ((c3 * t) >> 12) + c2;
    v = ((v * t) >> 12) + c1;
    v = ((v * t) >> 12);
    return p1 + (v >> 1);
}

/* limitador suave: linear ate 22000, depois curva assintotica para 32767 (A+B+PSG podem passar do fundo de escala) */
static inline int Clip16(int v)
{
    int m = v < 0 ? -v : v;
    if (m > 22000)
    {
        int e = m - 22000;
        m = 22000 + (10767 * e) / (e + 10767);
    }
    return v < 0 ? -m : m;
}

#define MAXIN 1024
void Ps2Audio_Submit(const int8_t *fifoA, const int8_t *fifoB, unsigned n, unsigned rate)
{
    static int bufA[3 + MAXIN], bufB[3 + MAXIN];   /* 3 amostras de historico + bloco novo (escala s16) */
    static int64_t pos = 1 << 16;                  /* 16.16, coordenadas de buf; p1 = buf[pos>>16] */
    static short out[2048 * 2] __attribute__((aligned(64)));
    static int fill = -1;                          /* nivel filtrado do buffer do audsrv (amostras) */

    if (!sAudioOk || !n || !rate || n > MAXIN) return;

    Ps2Psg_Frame(OUT_RATE);

    for (unsigned i = 0; i < n; i++)
    {
        bufA[3 + i] = fifoA[i] * 256;
        bufB[3 + i] = fifoB[i] * 256;
    }

    /* controle de taxa: mantem ~1 quadro de audio enfileirado */
    int step = (int)(((int64_t)rate << 16) / OUT_RATE);
    {
        int q = audsrv_queued();
        if (q >= 0)
        {
            int frames = q >> 2;
            fill = fill < 0 ? frames : (fill * 7 + frames) >> 3;
            const int target = 900;
            int e = ((fill - target) * 1310) / target;   /* +-2% */
            if (e > 1310) e = 1310;
            if (e < -1310) e = -1310;
            step = (int)(((int64_t)step * (65536 + e)) >> 16);
        }
    }

    unsigned ctl = gIoMem[0x82] | (gIoMem[0x83] << 8);
    int master = (gIoMem[0x84] & 0x80) != 0;
    /* DirectSound no GBA: 8 bits x2 (100%) ou x1 (50%) no mixer de 10 bits => A8*128 ou A8*64 em escala s16 */
    int gA = (ctl & 4) ? 128 : 64, gB = (ctl & 8) ? 128 : 64;
    int aL = (ctl >> 9) & 1, aR = (ctl >> 8) & 1, bL = (ctl >> 13) & 1, bR = (ctl >> 12) & 1;
    int psg = master && Ps2Psg_Active();
    const int vM = AudioGain(gAudioVol[0]), vD = AudioGain(gAudioVol[1]), vP = AudioGain(gAudioVol[2]);

    unsigned m = 0;
    while ((pos >> 16) <= (int64_t)n && m < 2048)
    {
        int k = (int)(pos >> 16);
        int t = (int)((pos >> 4) & 0xFFF);
        int a = Cubic(bufA[k - 1 + 0], bufA[k], bufA[k + 1], bufA[k + 2], t);
        int b = Cubic(bufB[k - 1 + 0], bufB[k], bufB[k + 1], bufB[k + 2], t);
        int l = 0, r = 0;
        if (master)
        {
            a = (((a * gA) >> 8) * vD) >> 8;
            b = (((b * gB) >> 8) * vD) >> 8;
            if (aL) l += a;
            if (aR) r += a;
            if (bL) l += b;
            if (bR) r += b;
            if (psg)
            {
                int pl, pr;
                Ps2Psg_Render(&pl, &pr);
                l += (pl * vP) >> 8;
                r += (pr * vP) >> 8;
#ifdef AUDIO_DUMP
                { static FILE *pf; static unsigned pt; if (!pf) pf = fopen("host:psg_dump.raw", "wb"); if (pf && pt < (6u << 20)) { short pp[2] = {(short)pl, (short)pr}; fwrite(pp, 4, 1, pf); pt += 4; if (pt >= (6u << 20)) fclose(pf); } }
#endif
            }
        }
        if (gAudioMono) l = r = (l + r) >> 1;
        l = (l * vM) >> 8; r = (r * vM) >> 8;
        out[m * 2] = (short)Clip16(l);
        out[m * 2 + 1] = (short)Clip16(r);
        m++;
        pos += step;
    }

    /* o bloco novo vira historico: as 3 ultimas amostras passam para o inicio */
    pos -= (int64_t)n << 16;
    if (pos < (1 << 16)) pos = 1 << 16;   /* p1 = buf[pos>>16] precisa de 1 amostra de historico antes (antes lia buf[-1]) */
    for (int i = 0; i < 3; i++)
    {
        bufA[i] = bufA[n + i];
        bufB[i] = bufB[n + i];
    }

#ifdef AUDIO_DUMP   /* teste no PCSX2: grava o PCM final em host:audio_dump.raw (s16 estereo 48 kHz) */
    { static FILE *f; static unsigned total; if (!f) f = fopen("host:audio_dump.raw", "wb"); if (f && total < (12u << 20)) { fwrite(out, 4, m, f); total += m * 4; if (total >= (12u << 20)) fclose(f); } }
#endif
    if (m)
    {
        /* nunca bloqueia o loop principal esperando o audsrv: se nao cabe, descarta o excedente */
        int room = audsrv_available() & ~3, bytes = (int)(m * 4);
        if (bytes > room) bytes = room;
        if (bytes > 0) audsrv_play_audio((const char *)out, bytes);
    }
}
