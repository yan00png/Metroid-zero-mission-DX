/* Relatorio de crash para hardware real: o PCSX2 aceita acessos desalinhados e outros erros que o EE real
 * transforma em excecao. Aqui capturamos a excecao (ou um travamento do loop principal) e mostramos
 * EPC/BadVAddr/registradores na tela para localizar a instrucao exata (addr2line no mesmo ELF). */
#include <tamtypes.h>
#include <kernel.h>
#include <semaphore.h>
#include <ps2_debug.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <debug.h>
#include <ee_debug.h>
#include "platform.h"
#ifndef CRASHTEST
#define CRASHTEST 0
#endif

static volatile uint32_t sBeat;          /* incrementado a cada PlatformWaitVBlank */
static volatile uint32_t sLastBeat, sStall, sHangEpc, sHangRa;
static volatile uint32_t sVbl;
static volatile uint32_t sFixN, sFixPc[8], sFixAddr[8];
static volatile int sExc;   /* a tela de crash esta (ou vai estar) na TV: a thread de travamento nao pode escrever por cima */
static int sMainTid = -1;
static volatile int sHang, sHangSema = -1, sVbId = -1;
static char sThreadStack[4096] __attribute__((aligned(16)));

static const char *ExcName(int c)
{
    switch (c)
    {
    case 1: return "TLB mod"; case 2: return "TLB miss LOAD/FETCH"; case 3: return "TLB miss STORE";
    case 4: return "ADDR ERR LOAD/FETCH"; case 5: return "ADDR ERR STORE"; case 6: return "BUS ERR FETCH";
    case 7: return "BUS ERR DATA"; case 9: return "BREAK"; case 10: return "RESERVED INSTR";
    case 12: return "OVERFLOW"; case 13: return "TRAP"; default: return "?";
    }
}

static void Show(const char *title, const char *l1, const char *l2, const char *l3, const char *l4, const char *l5)
{
    init_scr();
    scr_clear();
    scr_printf("\n  MZM PS2 - RELATORIO DE CRASH\n\n  %s\n\n", title);
    scr_printf("  %s\n  %s\n  %s\n  %s\n  %s\n", l1, l2, l3, l4, l5);
    scr_printf("\n  Tire uma foto desta tela e envie.\n");
}

static struct st_EE_RegFrame sSnap;
static char sCrashStack[8192] __attribute__((aligned(16)));

/* roda em contexto normal (apos o ERET), com pilha propria */
static void CrashScreen(void)
{
    struct st_EE_RegFrame *r = &sSnap;
    char a[96], b[96], c[96], d[96], e[96];
    int code = (r->cause >> 2) & 0x1F;
    snprintf(a, sizeof a, "EPC=%08X BADVADDR=%08X", (unsigned)r->epc, (unsigned)r->badvaddr);
    snprintf(b, sizeof b, "RA=%08X SP=%08X GP=%08X", (unsigned)r->ra[0], (unsigned)r->sp[0], (unsigned)r->gp[0]);
    snprintf(c, sizeof c, "A0=%08X A1=%08X A2=%08X A3=%08X", (unsigned)r->a0[0], (unsigned)r->a1[0], (unsigned)r->a2[0], (unsigned)r->a3[0]);
    snprintf(d, sizeof d, "V0=%08X V1=%08X T0=%08X T1=%08X", (unsigned)r->v0[0], (unsigned)r->v1[0], (unsigned)r->t0[0], (unsigned)r->t1[0]);
    char dbg[64]; PlatformDebugLine(dbg, sizeof dbg);
    snprintf(e, sizeof e, "%s", dbg);
    char t[64]; snprintf(t, sizeof t, "EXCECAO %d: %s  CAUSE=%08X", code, ExcName(code), (unsigned)r->cause);
    sHang = 1;
    {
        char f[96]; snprintf(f, sizeof f, "FIXUPS=%u: %08X@%08X %08X@%08X", (unsigned)sFixN, (unsigned)sFixPc[0], (unsigned)sFixAddr[0], (unsigned)sFixPc[1], (unsigned)sFixAddr[1]);
        Show(t, a, b, c, d, e);
        scr_printf("  %s\n", f);
    }
    for (;;) {}
}

/* ---- Acessos desalinhados: o EE real levanta AdEL/AdES onde o PCSX2 aceita calado. O codigo do jogo (decomp, ponteiros u8*
 * convertidos para u16 ou u32) tem muitos. Em vez de cair na tela de crash, emulamos a instrucao byte a byte e seguimos
 * (lh/lhu/lw/lwu/ld/sh/sw/sd, tambem no delay slot de um desvio). So o que nao da para emular (FPU lwc1/swc1, instrucao
 * desconhecida) chega na tela de crash. ---- */

static inline uint32_t *RGP(struct st_EE_RegFrame *r, int i) { return &((uint32_t *)r)[i * 4]; }

/* avalia o desvio em `pc` antes do delay slot; devolve 0 se nao for um desvio conhecido */
static int EvalBranch(struct st_EE_RegFrame *r, uint32_t pc, uint32_t *next)
{
    uint32_t ins = *(volatile uint32_t *)(uintptr_t)pc;
    int op = ins >> 26, rs = (ins >> 21) & 31, rt = (ins >> 16) & 31;
    int32_t a = (int32_t)*RGP(r, rs), b = (int32_t)*RGP(r, rt);
    uint32_t tgt = pc + 4 + ((uint32_t)(int32_t)(int16_t)(ins & 0xFFFF) << 2);
    int take;
    switch (op)
    {
    case 4: case 20: take = a == b; break;
    case 5: case 21: take = a != b; break;
    case 6: case 22: take = a <= 0; break;
    case 7: case 23: take = a > 0; break;
    case 1:
        switch (rt)
        {
        case 0: case 2: take = a < 0; break;
        case 1: case 3: take = a >= 0; break;
        default: return 0;
        }
        break;
    case 2: *next = ((pc + 4) & 0xF0000000u) | ((ins & 0x3FFFFFF) << 2); return 1;
    case 3:
        *next = ((pc + 4) & 0xF0000000u) | ((ins & 0x3FFFFFF) << 2);
        *RGP(r, 31) = pc + 8; RGP(r, 31)[1] = 0;
        return 1;
    case 0:
        if ((ins & 0x3F) == 8) { *next = (uint32_t)a; return 1; }
        if ((ins & 0x3F) == 9)
        {
            int rd = (ins >> 11) & 31; uint32_t t = (uint32_t)a;
            if (rd) { *RGP(r, rd) = pc + 8; RGP(r, rd)[1] = 0; }
            *next = t; return 1;
        }
        return 0;
    default: return 0;
    }
    *next = take ? tgt : pc + 8;
    return 1;
}

static int EmuMem(struct st_EE_RegFrame *r, uint32_t ins, int nullPage)
{
    int op = ins >> 26, rs = (ins >> 21) & 31, rt = (ins >> 16) & 31;
    uint32_t addr = *RGP(r, rs) + (uint32_t)(int32_t)(int16_t)(ins & 0xFFFF);
    if (addr != r->badvaddr) return 0;
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)addr;
    uint32_t lo = 0, hi = 0; int n;
    switch (op)
    {
    case 0x20: case 0x24: n = 1; break;
    case 0x28: n = 1; break;
    case 0x21: case 0x25: n = 2; break;
    case 0x23: case 0x27: n = 4; break;
    case 0x37: case 0x3F: n = 8; break;
    case 0x29: n = 2; break;
    case 0x2B: n = 4; break;
    default: return 0;
    }
    if (op < 0x28 || op == 0x37)   /* carga */
    {
        if (!rt) return 1;
        if (nullPage) { RGP(r, rt)[0] = 0; RGP(r, rt)[1] = 0; return 1; }   /* GBA le a BIOS/ROM em 0: aqui vale 0 */
        for (int i = 0; i < n; i++) { if (i < 4) lo |= (uint32_t)m[i] << (8 * i); else hi |= (uint32_t)m[i] << (8 * (i - 4)); }
        uint32_t *d = RGP(r, rt);
        if (n == 1) { if (op == 0x20) { lo = (uint32_t)(int32_t)(int8_t)lo; hi = (int32_t)lo >> 31; } else hi = 0; }
        else if (n == 2) { if (op == 0x21) { lo = (uint32_t)(int32_t)(int16_t)lo; hi = (int32_t)lo >> 31; } else hi = 0; }
        else if (n == 4) { hi = op == 0x23 ? (uint32_t)((int32_t)lo >> 31) : 0; }
        d[0] = lo; d[1] = hi;
        return 1;
    }
    if (nullPage) return 1;   /* escrita na pagina 0: descarta */
    {   /* escrita */
        const uint32_t *sv = RGP(r, rt);
        for (int i = 0; i < n; i++) m[i] = (uint8_t)((i < 4 ? sv[0] >> (8 * i) : sv[1] >> (8 * (i - 4))) & 0xFF);
        return 1;
    }
}

static int TryFixup(struct st_EE_RegFrame *r, int nullPage)
{
    uint32_t pc = r->epc, next = pc + 4, slot = pc;
    if (r->cause & 0x80000000u)   /* excecao no delay slot: EPC aponta para o desvio */
    {
        if (!EvalBranch(r, pc, &next)) return 0;
        slot = pc + 4;
    }
    if (!EmuMem(r, *(volatile uint32_t *)(uintptr_t)slot, nullPage)) return 0;
    r->epc = next;
    uint32_t k = sFixN++;
    if (k < 8) { sFixPc[k] = slot; sFixAddr[k] = r->badvaddr; }
    return 1;
}

static int OnException(struct st_EE_RegFrame *r)
{
    int code = (r->cause >> 2) & 0x1F;
    if ((code == 4 || code == 5) && TryFixup(r, 0)) return 0;
    /* ponteiro nulo (pagina 0): o GBA le a BIOS e nao acontece nada; o EE da TLB miss. Carga devolve 0, escrita e descartada. */
    if ((code == 2 || code == 3) && r->badvaddr < 0x10000 && TryFixup(r, 1)) return 0;
    sExc = 1;
    sSnap = *r;
    r->epc = (uint32_t)(uintptr_t)CrashScreen;
    r->sp[0] = (uint32_t)(uintptr_t)(sCrashStack + sizeof sCrashStack - 64);
    r->ra[0] = 0;
    return 0;
}

/* roda no vblank: se o loop principal nao bate ha ~4 s, guarda o PC interrompido e acorda a thread de relatorio */
static volatile uint32_t sCand[6];   /* amostras brutas: addr (3a arg do handler) e EPC lido no handler */
static volatile uint32_t sAddrRaw[3], sCtxPc[6];
static int OnVblank(int cause, void *arg, void *addr)
{
    (void)cause; (void)arg; sVbl++;
    if (sBeat != sLastBeat) { sLastBeat = sBeat; sStall = 0; }
    else if (!sHang)
    {
        sStall++;
        if (sStall >= 200 && sStall < 203) sAddrRaw[sStall - 200] = (uint32_t)(uintptr_t)addr;
        if (sStall == 203 && addr)
        {   /* palavras do contexto salvo que parecem enderecos de codigo do ELF */
            uint32_t p = (uint32_t)(uintptr_t)addr;
            if (p >= 0x100000 && p < 0x2000000)
            {
                const uint32_t *w = (const uint32_t *)(uintptr_t)p; int k = 0;
                for (int i = 0; i < 96 && k < 6; i++) if (w[i] >= 0x100000 && w[i] < 0x800000 && !(w[i] & 3)) sCtxPc[k++] = w[i];
            }
        }
        if (sStall > 240)
        {
            uint32_t epc, ra;
            __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
            __asm__ volatile("move %0, $31" : "=r"(ra));
            sHangEpc = epc; sHangRa = ra; sHang = 1;
            iSignalSema(sHangSema);
        }
    }
    return 0;
}

static volatile uint32_t sPost[8], sPostN, sPostLast;
static volatile uint32_t sPcRing[64], sPcN, sPcLast, sTimStall, sTimBeat, sTicks;
/* timer 0 do EE a 10 Hz: independe do vblank; o 3o argumento e o PC interrompido */
static int OnTimer(int cause, void *arg, void *addr)
{
    (void)cause; (void)arg;
    *(volatile uint32_t *)0x10000010 |= 0x400;   /* limpa a flag de comparacao */
    sTicks++;
    {   /* trilha: so guarda o PC quando muda de bloco de 64 bytes (esconde o spin do vsync) */
        uint32_t pc = (uint32_t)(uintptr_t)addr;
        if (!sHang && (pc >> 6) != (sPcLast >> 6)) { sPcRing[sPcN++ & 63] = pc; sPcLast = pc; }
        if (sHang && sPostN < 8 && pc != sPostLast) { sPost[sPostN++] = pc; sPostLast = pc; }
    }
    if (sBeat != sTimBeat) { sTimBeat = sBeat; sTimStall = 0; }
    else if (++sTimStall == 8000 && !sHang) { sHang = 1; iSignalSema(sHangSema); }
    return 0;
}

static void ReportThread(void *arg)
{
    (void)arg;
    for (;;)
    {
        DelayThread(500000);
        if (sExc) for (;;) SleepThread();   /* excecao: a tela dela fica */
        if (sHang) break;
#if CRASHTEST
        { FILE *f = fopen("host:/tmp/ps2/crash_poll.txt", "w"); if (f) { fprintf(f, "beat=%u vbl=%u stall=%u\n", (unsigned)sBeat, (unsigned)sVbl, (unsigned)sStall); fclose(f); } }
#endif
        static uint32_t last, still;
        if (sBeat != last) { last = sBeat; still = 0; }
        else if (++still >= 40) { sHang = 1; break; } /* 20 s: so se o vblank nao disparou */
    }
#if CRASHTEST
    { FILE *f = fopen("host:/tmp/ps2/crash.txt", "w"); if (f) { fprintf(f, "hang epc=%08x ra=%08x\n", (unsigned)sHangEpc, (unsigned)sHangRa); fclose(f); } }
#endif
    if (sExc) for (;;) SleepThread();
    /* sonda do CRTC (antes de init_scr): 1a fase so le o CSR; 2a limpa VSINT a cada 25 ms */
    uint32_t beat0 = sBeat, vbl0 = sVbl;
    int ro = 0, vs = 0, fl = 0, vb = 0; uint32_t f0 = (*(volatile uint32_t *)0x12001000 >> 13) & 1;
    for (int i = 0; i < 20; i++)
    {
        DelayThread(25000);
        uint32_t csr = *(volatile uint32_t *)0x12001000;
        if (csr & 8) ro = 1;
    }
    for (int i = 0; i < 20; i++)
    {
        *(volatile uint32_t *)0x12001000 = 8; /* limpa VSINT */
        DelayThread(25000);
        uint32_t csr = *(volatile uint32_t *)0x12001000, f = (csr >> 13) & 1;
        if (csr & 8) vs++;
        if (f != f0) { fl++; f0 = f; }
        if (*(volatile uint32_t *)0x1000F000 & 4) vb++;
    }
    uint32_t beat1 = sBeat, vbl1 = sVbl;
    char a[96], b[96], e[96];
    snprintf(a, sizeof a, "INTERROMPIDO EM EPC=%08X  RA=%08X", (unsigned)sHangEpc, (unsigned)sHangRa);
    snprintf(b, sizeof b, "TRILHA (recente primeiro):");
    PlatformDebugLine(e, sizeof e);
    char c[96]; snprintf(c, sizeof c, "CTX: %08X %08X %08X %08X", (unsigned)sCtxPc[0], (unsigned)sCtxPc[1], (unsigned)sCtxPc[2], (unsigned)sCtxPc[3]);
    ee_thread_status_t ts; memset(&ts, 0, sizeof ts);
    int rc = ReferThreadStatus(sMainTid, &ts);
    char g[96]; snprintf(g, sizeof g, "MAIN: rc=%d st=%d wt=%d VBL=%u BEAT=%u TICKS=%u", rc, ts.status, ts.waitType, (unsigned)sVbl, (unsigned)sBeat, (unsigned)sTicks);
    #define R32(a) (*(volatile uint32_t *)(a))
    snprintf(c, sizeof c, "D2 CHCR=%08X MADR=%08X QWC=%08X TADR=%08X", (unsigned)R32(0x1000A000), (unsigned)R32(0x1000A010), (unsigned)R32(0x1000A020), (unsigned)R32(0x1000A030));
    char h[96]; snprintf(h, sizeof h, "GIF_STAT=%08X GS_CSR=%08X INTC_STAT=%08X MASK=%08X", (unsigned)R32(0x10003020), (unsigned)R32(0x12001000), (unsigned)R32(0x1000F000), (unsigned)R32(0x1000F010));
    char th[160]; th[0] = 0; int tl = 0;
    for (int id = 1; id < 40 && tl < 130; id++)
    {
        ee_thread_status_t t2; memset(&t2, 0, sizeof t2);
        if (ReferThreadStatus(id, &t2) < 0) continue;
        if (t2.status == 0x10) continue; /* dormant */
        tl += snprintf(th + tl, sizeof th - tl, "%d:s%d p%d w%d | ", id, t2.status, t2.current_priority, t2.waitType);
    }
    init_scr(); scr_clear();
    scr_printf("\n  THREADS: %s\n", th);
    scr_printf("\n  MZM PS2 - TRAVAMENTO  CRTC: leitura=%d vsint=%d/20 field=%d vbon=%d\n  BEAT %u>%u VBL %u>%u PC_POS: %08X %08X %08X %08X\n  %s\n  %s\n  %s\n  %s\n  TRILHA DE PC (recente primeiro):\n", ro, vs, fl, vb, (unsigned)beat0, (unsigned)beat1, (unsigned)vbl0, (unsigned)vbl1, (unsigned)sPost[0], (unsigned)sPost[1], (unsigned)sPost[2], (unsigned)sPost[3], e, g, c, h);
    for (int row = 0; row < 4; row++)
    {
        scr_printf("  ");
        for (int k = 0; k < 6; k++) scr_printf("%08X ", (unsigned)sPcRing[(sPcN - 1 - row * 6 - k) & 63]);
        scr_printf("\n");
    }
    scr_printf("  D_STAT=%08X D_CTRL=%08X  FIXUPS=%u\n  Tire uma foto desta tela e envie.\n", (unsigned)R32(0x1000E010), (unsigned)R32(0x1000E000), (unsigned)sFixN);
    for (;;) SleepThread();
}

void PlatformBeat(void) { sBeat++; }

static int sRepTid = -1, sTimId = -1;

/* Antes de abrir outro ELF: para o timer de 2 kHz, tira os handlers de vblank/timer e as excecoes de depuracao, mata a thread
 * de relatorio. Tudo isso aponta para codigo deste programa, que o ELF novo sobrescreve: um tick do timer durante a copia
 * (ou depois, ja no programa novo) cai em codigo errado e a tela fica branca. */
void CrashShutdown(void)
{
    DIntr();
    *(volatile uint32_t *)0x10000010 = 0;   /* T0_MODE: para o timer */
    *(volatile uint32_t *)0x10000000 = 0;
    *(volatile uint32_t *)0x10000010 = 0x400 | 0x800;   /* limpa flags de comparacao/overflow */
    DisableIntc(INTC_TIM0);
    DisableIntc(INTC_VBLANK_S);
    if (sTimId >= 0) RemoveIntcHandler(INTC_TIM0, sTimId);
    if (sVbId >= 0) RemoveIntcHandler(INTC_VBLANK_S, sVbId);
    sTimId = sVbId = -1;
    ee_dbg_install(0);
    if (sRepTid >= 0) { TerminateThread(sRepTid); DeleteThread(sRepTid); sRepTid = -1; }
    EIntr();
}

void CrashInit(void)
{
    sMainTid = GetThreadId();
    ChangeThreadPriority(sMainTid, 3); /* a thread de relatorio (prio 1) precisa poder interromper o loop principal */
    ee_dbg_install(1);
    static const int codes[] = {1, 2, 3, 4, 5, 6, 7, 9, 10, 12, 13};
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++) ee_dbg_set_level1_handler(codes[i], OnException);

    ee_sema_t s = {0}; s.init_count = 0; s.max_count = 1;
    sHangSema = CreateSema(&s);
    ee_thread_t t = {0};
    t.func = ReportThread; t.stack = sThreadStack; t.stack_size = sizeof sThreadStack;
    t.gp_reg = &_gp; t.initial_priority = 1;
    int id = CreateThread(&t);
    sRepTid = id;
    if (id >= 0) StartThread(id, NULL);
#if CRASHTEST
    { FILE *f = fopen("host:/tmp/ps2/crash_init.txt", "w"); if (f) { fprintf(f, "init thread=%d sema=%d\n", id, sHangSema); fclose(f); } }
#endif
    sVbId = AddIntcHandler2(INTC_VBLANK_S, OnVblank, -1, NULL);
    sTimId = AddIntcHandler2(INTC_TIM0, OnTimer, -1, NULL);
    *(volatile uint32_t *)0x10000020 = 288;       /* COMP: ~2 kHz (BUSCLK/256) */
    *(volatile uint32_t *)0x10000000 = 0;
    *(volatile uint32_t *)0x10000010 = 0x582;     /* clk=BUSCLK/256, CUE, CMPE, ZRET */
    EnableIntc(INTC_TIM0);
    EnableIntc(INTC_VBLANK_S);
}
