/* Motor de som de Metroid: Zero Mission para o port PS2.
 *
 * Escrito do zero a partir do assembly ORIGINAL do jogo (decomp mzm: asm/audio_internal.s e asm/soundcode.s)
 * e do hardware de som do GBA. Nao usa codigo do port do Switch/NX (GPL) nem do port do 3DS.
 * Substitui as rotinas ARM/Thumb: faixas e instrumentos (InitTrack, unk_4e10...), mixer de amostras de 8 bits
 * (SoundCodeA/B/C) e a ponte dos registradores PSG (unk_5104 -> gIoMem, lidos por ps2_psg.c).
 *
 * Convencoes do port: os ponteiros dentro dos dados de som sao enderecos GBA (0x08xxxxxx) brutos e sao
 * resolvidos com port_resolve_addr(). Os campos 'unk_11/unk_12' do canal sao os volumes das FIFOs A e B
 * (A -> alto-falante direito, B -> esquerdo; ver SOUNDCNT_H = 0xA90E em audio_wrappers.c).
 */
#include "audio.h"
#include "audio/track_internal.h"
#include "data/audio.h"
#include "port_gba_mem.h"
#include "syscalls.h"
#include <stdint.h>
#include <string.h>
#include "ps2_psg.h"

extern void *port_resolve_addr(uintptr_t val);
extern void Ps2Audio_Submit(const int8_t *fifoA, const int8_t *fifoB, unsigned n, unsigned rate);
void DoSoundAction(u32 action);

static uint32_t Rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------------------------------------ */
/* Tabela de instrumentos                                                                            */
/* ------------------------------------------------------------------------------------------------ */

/* cada instrumento ocupa 12 bytes na ROM: tipo, tecla/pitch, byte 2, byte 3, ponteiro da amostra, envelope */
void NativeAudio_GetVoice(const struct Voice *group, unsigned index, struct Voice *voice)
{
    const uint8_t *e = (const uint8_t *)port_resolve_addr((uintptr_t)group) + index * 12;
    voice->instrumentType = e[0];
    voice->pitch = e[1];
    voice->unk_2 = e[2];
    voice->unk_3 = e[3];
    voice->pSample = (u32 *)(uintptr_t)Rd32(e + 4);
    memcpy(&voice->envelope, e + 8, 4);
}

/* ------------------------------------------------------------------------------------------------ */
/* Faixas                                                                                            */
/* ------------------------------------------------------------------------------------------------ */

/* solta todos os canais de amostra e de PSG usados pelas vozes da faixa (corpo comum de ResetTrack/StopMusicOrSound) */
static void ReleaseTrackVoices(struct TrackData *t)
{
    if (t->unk_1E & 1) return;
    if (!(t->flags & 2)) return;
    t->flags = 1;
    t->currentTrack = 0;
    for (unsigned i = 0; i < t->amountOfTracks; i++)
    {
        struct TrackVariables *v = &t->pVariables[i];
        struct PsgSoundData *psg = v->pSoundPsg;
        if (psg)
        {
            ClearRegistersForPsg(psg, (v->channel & 7) - 1);
            psg->unk_0 = 0;
            psg->pVariables = NULL;
        }
        for (struct SoundChannel *c = v->pChannel; c;)
        {
            struct SoundChannel *next = c->pChannel2;
            c->unk_0 = 0;
            c->pVariables = NULL;
            c->pChannel2 = NULL;
            c->pChannel1 = NULL;
            c = next;
        }
        v->pSoundPsg = NULL;
        v->pChannel = NULL;
    }
}

void ResetTrack(struct TrackData *t)
{
    ReleaseTrackVoices(t);
}

void StopMusicOrSound(struct TrackData *t)
{
    if (t->occupied) return;
    t->occupied = 1;
    ReleaseTrackVoices(t);
    t->occupied = 0;
}

/* devolve 1 se a faixa atual tem prioridade maior e o pedido foi recusado */
u8 InitTrack(struct TrackData *t, const u32 *header)
{
    const uint8_t *h = (const uint8_t *)port_resolve_addr((uintptr_t)header);
    if (!h || t->occupied) return 0;
    t->occupied = 1;

    if (t->unk_1E & 1)
    {
        t->occupied = 0;
        return 0;
    }
    if (h[0] == 0) /* cabecalho sem faixas = parar */
    {
        ReleaseTrackVoices(t);
        t->flags = 0;
        t->occupied = 0;
        return 0;
    }
    if ((t->flags & 2) && t->unk_1D && t->unk_3 > h[2])
    {
        t->occupied = 0;
        return 1;
    }
    ReleaseTrackVoices(t);

    unsigned count = h[0];
    if (count > t->maxAmountOfTracks) count = t->maxAmountOfTracks;
    t->flags = 2;
    t->amountOfTracks = (u8)count;
    t->undefined_2 = h[1];
    t->unk_3 = h[2];
    t->pVoice = (struct Voice *)(uintptr_t)Rd32(h + 4);
    t->pHeader = (const u8 *)header;
    t->unk_C = 256;
    if (h[3] & 0x80) DoSoundAction(h[3]);

    memset(t->pVariables, 0, count * sizeof(struct TrackVariables));
    for (unsigned i = 0; i < count; i++)
    {
        struct TrackVariables *v = &t->pVariables[i];
        v->unk_0 = 1;
        v->bendRange = 2;
        v->unk_1B = 12;
        v->unk_5 = 0x40;
        v->unk_6 = 0x40;
        v->pRawData = (const u8 *)port_resolve_addr(Rd32(h + 8 + i * 4));
    }
    t->occupied = 0;
    return 0;
}

/* acumulador de andamento: devolve quantos "ticks" da musica passam neste quadro */
u32 unk_4cfc(struct TrackData *t)
{
    uint32_t total = ((uint32_t)t->unk_C + t->unk_E) & 0xFFFF;
    t->unk_E = (u16)(total & 0xFF);
    return total >> 8;
}

/* comando de andamento (tempo) */
void unk_4d1c(struct TrackData *t, struct TrackVariables *v)
{
    uint32_t bpm2 = (uint32_t)v->pRawData[1] * 2;
    v->pRawData += 2;
    t->unk_A = (u16)bpm2;
    t->unk_C = (bpm2 == 150) ? 256 : (u16)(((bpm2 << 8) * 0x1B4Eu) >> 20);
    t->unk_E = 0;
}

/* chamado uma vez por quadro */
void UpdateAudio(void)
{
    for (unsigned i = 0; i < 9; i++)
    {
        if (!gSoundQueue[i].exists) continue;
        InitTrack(sMusicTrackDataRom[i].pTrack, (const u32 *)gSoundQueue[i].pHeader);
        memset(&gSoundQueue[i], 0, sizeof gSoundQueue[i]);
    }

    unsigned prio = gMusicInfo.priority;
    unsigned mode = prio & 0x7F;
    if (prio & 0x80)
    {
        /* modo "so musica de fundo parada": as faixas de efeito (1..8) continuam, a 0 nao */
        if (mode != 4)
            for (unsigned i = 1; i < 9; i++)
            {
                struct TrackData *t = sMusicTrackDataRom[i].pTrack;
                if (t && (t->flags & 2)) UpdateTrack(t);
            }
    }
    else if (mode == 4)
    {
        struct TrackData *t = sMusicTrackDataRom[gMusicInfo.unk_20 ? 1 : 0].pTrack;
        if (t && (t->flags & 2)) UpdateTrack(t);
    }
    else
    {
        for (unsigned i = 0; i < 9; i++)
        {
            struct TrackData *t = sMusicTrackDataRom[i].pTrack;
            if (t && (t->flags & 2)) UpdateTrack(t);
        }
    }

    if (!gMusicInfo.occupied)
    {
        gMusicInfo.occupied = 1;
        /* No GBA a posicao de reproducao (unk_10) avanca na interrupcao da DMA do som (sub_08004ad8).
         * Aqui nao ha DMA: avancamos exatamente um quadro de amostras por chamada. */
        if (gMusicInfo.unk_E)
        {
            gMusicInfo.unk_10 += gMusicInfo.unk_C;
            if (gMusicInfo.unk_10 >= gMusicInfo.unk_E) gMusicInfo.unk_10 -= gMusicInfo.unk_E;
        }
        UpdatePsgSounds();
        UpdateMusic();
    }
    gMusicInfo.occupied = 0;
}

/* versao 1-por-interrupcao do avanco (nao usada no port) */
void sub_08004ad8(void)
{
    if (++gMusicInfo.unk_10 >= gMusicInfo.unk_E) gMusicInfo.unk_10 = 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Instrumento -> variaveis da voz                                                                    */
/* ------------------------------------------------------------------------------------------------ */

void unk_4e10(struct TrackVariables *v)
{
    const uint8_t *entry = (const uint8_t *)v->pSample2;
    unsigned key = v->unk_1;

    if ((v->unk_0 & 0xF0) == 0x80) /* percussao: uma entrada por nota */
    {
        entry += key * 12;
        v->unk_35 = entry[1];
        if (entry[3] == 0)
            v->unk_6 = 0x40;
        else if (entry[3] >= 0x80)
            v->unk_6 = entry[3] - 0x80;
    }
    else /* multi-amostra: um mapa de tecla -> instrumento fica em envelope2 */
    {
        uint32_t mapAddr;
        memcpy(&mapAddr, &v->envelope2, 4);
        const uint8_t *map = (const uint8_t *)port_resolve_addr(mapAddr);
        entry += map[key] * 12;
    }

    uint8_t type = entry[0];
    v->channel = type;
    v->unk_36 = entry[2];
    uint32_t ptr = Rd32(entry + 4);

    switch (type & 7)
    {
    case 0: /* amostra PCM */
        v->pSample1 = (u32 *)port_resolve_addr(ptr);
        break;
    case 1:
    case 2: /* quadrado: duty em pSample1 (byte baixo), byte 3 = envelope do canal */
    {
        uint8_t b3 = entry[3];
        v->unk_37 = (!(b3 & 0x80) && (b3 & 0x70)) ? b3 : 8;
        v->pSample1 = (u32 *)(uintptr_t)((ptr << 6) & 0xFF);
        break;
    }
    case 3: /* onda programavel */
        UploadSampleToWaveRam((const u32 *)(uintptr_t)ptr);
        break;
    case 4: /* ruido */
        v->pSample1 = (u32 *)(uintptr_t)((ptr << 3) & 0xFF);
        break;
    default:
        break;
    }
    memcpy(&v->envelope1, entry + 8, 4);
}

/* afinacao da voz em 1/256 de semitom: unk_17 = parte inteira, unk_18 = fracao */
void unk_4eb4(struct TrackVariables *v)
{
    int bend = v->pitchBend;
    if (bend > 0) bend++;
    uint32_t w = (uint32_t)v->keyShift | ((uint32_t)v->unk_1D << 8) | ((uint32_t)v->tune << 16) | ((uint32_t)v->unk_1F << 24);

    int value = bend * v->bendRange * 4;
    value += (int8_t)(w & 0xFF) * 256;
    value += (int8_t)((w >> 8) & 0xFF) * 256;
    value += (((int32_t)(w << 8)) >> 22) * 4; /* sim: o original mistura 2 bits de unk_1D aqui */
    value += (int)(w >> 24);

    if (v->lfoSpeed && v->modulationDepth && v->modulationType == 0)
        value += (int8_t)v->unk_13 * 4 * 12;

    v->unk_17 = (s8)(value >> 8);
    v->unk_18 = (u8)value;
}

/* volume e panoramica: unk_8/unk_9 = volume das FIFOs A (direita) e B (esquerda) */
void unk_4f10(struct TrackVariables *v)
{
    unsigned vol = ((unsigned)v->volume * v->unk_5) >> 5;
    if (v->modulationType == 1)
    {
        int m = ((int)vol * ((int8_t)v->unk_13 + 0x41)) >> 6;
        vol = m < 0 ? 0 : m >= 0xFF ? 0xFF : (unsigned)m;
    }
    int pan = (int8_t)v->unk_7 + v->unk_6 - 0x40;
    if (pan >= 0x3F)
        pan = 0x3F;
    else if (pan < -0x40)
        pan = 0x40; /* o original faz isso mesmo (bug do jogo) */
    if (v->modulationType == 2)
    {
        pan += (int8_t)v->unk_13;
        if (pan >= 0x3F)
            pan = 0x3F;
        else if (pan + 0x40 < 0)
            pan = -0x40;
    }
    v->unk_8 = (u8)(((64 + pan) * vol) >> 7);
    v->unk_9 = (u8)(((64 - pan) * vol) >> 7);
}

/* ------------------------------------------------------------------------------------------------ */
/* Altura: substitui a chamada de BIOS MidiKey2Freq e a multiplicacao 32x32 do mixer                    */
/* ------------------------------------------------------------------------------------------------ */

/* 2^(i/12) em 16.16 */
static const uint32_t kSemi[12] = {65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715};

/* BIOS: freq = wave.freq / 2^((180 - tecla - fino/256) / 12); wave.freq e a palavra 1 do cabecalho da amostra */
u32 MidiKey2Freq(u32 *waveData, u8 midiKey, u8 fineAdjust)
{
    if (!waveData) return 0;
    const uint32_t *wd = (const uint32_t *)port_resolve_addr((uintptr_t)waveData);
    uint64_t base = wd[1];
    if (!base) return 0;

    int units = ((int)midiKey - 180) * 256 + fineAdjust; /* em 1/256 de semitom, <= 0 para teclas validas */
    int oct = units >= 0 ? units / 3072 : -((-units + 3071) / 3072);
    int rem = units - oct * 3072;                          /* 0..3071 */
    unsigned semi = (unsigned)rem >> 8, fine = (unsigned)rem & 0xFF;

    /* 2^(fine/3072) ~ 1 + a + a^2/2, a = ln2*fine/3072 (erro < 0.01%) */
    uint32_t a = (fine * 3786u) >> 8; /* ln2/12 em 16.16 = 3786, vezes fine/256 */
    uint64_t mult = kSemi[semi];
    mult = (mult * (65536u + a + ((a * a) >> 17))) >> 16; /* 16.16 */

    uint64_t f = base * mult; /* 16.16 */
    if (oct >= 0)
        f <<= oct;
    else if (-oct < 63)
        f >>= -oct;
    else
        f = 0;
    return (u32)((f + 0x8000) >> 16);
}

u32 CallGetNoteFrequency(u32 key, u32 multiplier)
{
    return (u32)(((uint64_t)key * multiplier) >> 18);
}

/* liga uma voz da faixa a um canal de amostra livre */
void unk_4f8c(struct SoundChannel *c, struct TrackVariables *v, s32 priority)
{
    c->unk_13 |= 2;
    unk_20a4(c);

    if (v->pChannel) v->pChannel->pChannel1 = c;
    c->pChannel2 = v->pChannel;
    c->pChannel1 = NULL;
    c->pVariables = v;
    v->pChannel = c;

    c->unk_0 = 1;
    c->unk_1 = v->channel;
    c->unk_2 = (u8)priority;
    c->unk_3 = v->unk_6;

    c->envelope = v->envelope1;
    c->unk_C = v->unk_C;
    c->unk_D = v->unk_D;
    c->unk_E = v->unk_E;
    c->unk_F = v->unk_F;

    c->pSample = v->pSample1;
    c->pData = (u8 *)v->pSample1 + 16;
    c->pSize = v->pSample1 + 3;

    unsigned key = v->unk_1;
    c->unk_6 = (u8)key;
    if ((v->unk_0 & 0xF0) == 0x80) key = v->unk_35;
    c->unk_7 = (u8)key;

    int note = (int8_t)v->unk_17 + (int)key;
    if (note < 0) note = 0;
    if (note > 0x7F) note = 0x7F;

    u32 freq = MidiKey2Freq(v->pSample1, (u8)note, v->unk_18);
    c->unk_1C = (freq == gMusicInfo.sampleRate) ? 0x4000 : CallGetNoteFrequency(freq, gMusicInfo.pitch);
}

/* ------------------------------------------------------------------------------------------------ */
/* Comandos de sequencia                                                                              */
/* ------------------------------------------------------------------------------------------------ */

void AudioCommand_Goto(struct TrackVariables *v)
{
    v->pRawData = (const u8 *)port_resolve_addr(Rd32(v->pRawData + 1));
}

void AudioCommand_PatternPlay(struct TrackVariables *v)
{
    const uint8_t *ret = v->pRawData + 5;
    v->pRawData = (const u8 *)port_resolve_addr(Rd32(v->pRawData + 1));
    /* a pilha de retorno tem 3 niveis; se estiver cheia o original sobrescreve o ultimo */
    int slot = 2;
    for (int i = 0; i < 3; i++)
        if (!v->patternStartPointers[i])
        {
            slot = i;
            break;
        }
    v->patternStartPointers[slot] = ret;
}

/* ------------------------------------------------------------------------------------------------ */
/* Registradores PSG (lidos por ps2_psg.c)                                                           */
/* ------------------------------------------------------------------------------------------------ */

static void Io16(unsigned off, unsigned v)
{
    gIoMem[off] = (uint8_t)v;
    gIoMem[off + 1] = (uint8_t)(v >> 8);
}

void UploadSampleToWaveRam(const u32 *sample)
{
    const uint8_t *src = (const uint8_t *)port_resolve_addr((uintptr_t)sample);
    if (!src) return;
    gIoMem[0x70] = 0x40; /* seleciona o banco de escrita */
    memcpy(gIoMem + 0x90, src, 16);
    gIoMem[0x70] = 0;    /* o original deixa o canal 3 desligado ate o proximo unk_5104 */
}

void unk_5104(struct PsgSoundData *s)
{
    if (!s->pVariables) return;
    unsigned ch = s->pVariables->channel;
    unsigned freq = s->unk_14;

    if (ch > 8)
    {
        unsigned bias = gIoMem[0x89] & 0xC0; /* resolucao do PWM */
        if (bias < 0x40)
            freq = (freq + 2) & 0xC7FE;
        else if (bias < 0x80)
            freq = (freq + 1) & 0xC7FE;
    }

    unsigned lenEnv = s->unk_11 | ((unsigned)s->unk_12 << 8);
    switch (ch & 7)
    {
    case 1:
        gIoMem[0x60] = s->unk_10;
        Io16(0x62, lenEnv);
        Io16(0x64, freq);
        break;
    case 2:
        Io16(0x68, lenEnv);
        Io16(0x6C, freq);
        break;
    case 3:
        gIoMem[0x70] = s->unk_10;
        Io16(0x72, lenEnv);
        Io16(0x74, freq);
        break;
    case 4:
        Io16(0x78, lenEnv);
        Io16(0x7C, freq);
        break;
    default:
        return;
    }
    if (freq & 0x8000) Ps2Psg_Trigger(ch & 7);
    s->unk_14 = (u16)(freq & 0x7FFF);
}

/* ------------------------------------------------------------------------------------------------ */
/* Mixer (SoundCodeA/B/C)                                                                             */
/* ------------------------------------------------------------------------------------------------ */

/* B: realimentacao (reverb) a partir do quadro anterior do anel de saida; tambem zera/inicializa o acumulador */
u8 *CallSoundCodeB(u32 *left, u32 *right, u32 *mix, u8 count)
{
    const int8_t *A = (const int8_t *)left;
    const int8_t *B = (const int8_t *)right;
    int32_t *out = (int32_t *)mix;
    int reverb = gMusicInfo.reverb;
    for (unsigned i = 0; i < (unsigned)count * 2; i++)
    {
        int sum = A[i + PCM_DMA_BUF_SIZE] + B[i + PCM_DMA_BUF_SIZE] + A[i] + B[i];
        int v = (sum * reverb) >> 9;
        if (v < 0) v++; /* asrs + addmi do original */
        v *= 128;
        out[0] = out[1] = v;
        out += 2;
    }
    return (u8 *)out;
}

/* A: soma um canal de amostra (8 bits, com interpolacao linear) no acumulador estereo de 32 bits.
 * 'groups' = numero de blocos de 4 amostras. Termina o canal (unk_0 = 0) ao fim de uma amostra sem repeticao. */
void CallSoundCodeA(struct SoundChannel *ch, u32 *mixp, u8 groups)
{
    int32_t *mix = (int32_t *)mixp;
    const uint32_t *hdr = (const uint32_t *)ch->pSample;
    const int8_t *data = (const int8_t *)ch->pData;
    if (!hdr || !data) return;

    unsigned total = (unsigned)groups * 4;
    int32_t vA = ch->unk_11, vB = ch->unk_12;
    uint32_t len = hdr[3];
    uint32_t loopStart = hdr[2];
    int looping = (hdr[0] >> 16) != 0;
    uint32_t pos = ch->unk_18;

    if (ch->unk_1 == 0x08) /* modo direto: uma amostra por saida, posicao inteira */
    {
        for (unsigned i = 0; i < total; i++)
        {
            int s = data[pos];
            mix[i * 2] += s * vA;
            mix[i * 2 + 1] += s * vB;
            if (++pos >= len)
            {
                if (!looping)
                {
                    ch->unk_0 = 0;
                    return;
                }
                pos = loopStart;
            }
        }
    }
    else if (ch->unk_1 == 0x20) /* reverso */
    {
        uint32_t step = ch->unk_1C;
        for (unsigned i = 0; i < total; i++)
        {
            uint32_t idx = pos >> 14;
            int s0 = data[idx], s1 = data[idx + 1];
            int s = s0 + (((s1 - s0) * (int)(pos & 0x3FFF)) >> 14);
            mix[i * 2] += s * vA;
            mix[i * 2 + 1] += s * vB;
            if ((int32_t)(pos - step) < 0x4000)
            {
                ch->unk_0 = 0;
                return;
            }
            pos -= step;
        }
    }
    else /* normal: passo de ponto fixo 18.14 */
    {
        uint32_t step = ch->unk_1C;
        int32_t jump = (int32_t)(loopStart - len);
        for (unsigned i = 0; i < total; i++)
        {
            uint32_t idx = pos >> 14;
            int s0 = data[idx], s1 = data[idx + 1];
            int s = s0 + (((s1 - s0) * (int)(pos & 0x3FFF)) >> 14);
            mix[i * 2] += s * vA;
            mix[i * 2 + 1] += s * vB;
            pos += step;
            idx = pos >> 14;
            if (idx >= len)
            {
                if (!looping)
                {
                    ch->unk_0 = 0;
                    return;
                }
                pos = (pos & 0x3FFF) + ((uint32_t)((int32_t)idx + jump) << 14);
            }
        }
    }
    ch->unk_18 = pos;
}

/* C: acumulador de 32 bits -> amostras de 8 bits das FIFOs A e B (com o mesmo corte/wrap de 9 bits do
 * original) e entrega ao PS2. 'destination' tem A no inicio e B em +PCM_DMA_BUF_SIZE. */
u8 *CallSoundCodeC(u32 *destination, u32 *mixp, u8 groups)
{
    int8_t *A = (int8_t *)destination;
    int8_t *B = A + PCM_DMA_BUF_SIZE;
    const int32_t *mix = (const int32_t *)mixp;
    unsigned n = (unsigned)groups * 4;
    if (n * 2 > 768) return (u8 *)(mixp + 768); /* nao cabe no acumulador (nao acontece com os modos do jogo) */

    for (unsigned i = 0; i < n; i++)
    {
        int a = (mix[i * 2] >> 7) & 0x1FF;
        int b = (mix[i * 2 + 1] >> 7) & 0x1FF;
        if (a >= 0x100) a -= 0x200; /* 9 bits com sinal */
        if (b >= 0x100) b -= 0x200;
        A[i] = (int8_t)(a > 127 ? 127 : a < -128 ? -128 : a);
        B[i] = (int8_t)(b > 127 ? 127 : b < -128 ? -128 : b);
    }
    Ps2Audio_Submit(A, B, n, gMusicInfo.sampleRate);
    return (u8 *)(mixp + n * 2);
}
