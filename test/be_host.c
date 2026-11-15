/* Backend de teste no host: amostra as mesmas texturas/CLUTs que o GS receberia. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
static void OnBus(int sig, siginfo_t *si, void *uc_) {
    __asm__ volatile("pushf; andl $0xfffbffff,(%esp); popf");
    ucontext_t *uc = uc_; unsigned pc = uc->uc_mcontext.gregs[REG_EIP];
    static unsigned seen[4096]; static int n;
    int dup = 0; for (int i = 0; i < n; i++) if (seen[i] == pc) dup = 1;
    if (!dup && n < 4096) { seen[n++] = pc; fprintf(stderr, "UNALIGNED pc=%08x addr=%p\n", pc, si->si_addr); }
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x40000; uc->uc_mcontext.gregs[REG_EFL] |= 0x100; /* sem AC, 1 passo */
}
static void OnTrap(int sig, siginfo_t *si, void *uc_) {
    __asm__ volatile("pushf; andl $0xfffbffff,(%esp); popf");
    ucontext_t *uc = uc_; uc->uc_mcontext.gregs[REG_EFL] &= ~0x100; uc->uc_mcontext.gregs[REG_EFL] |= 0x40000;
}
#include "gfx_cache.h"
#include "gfx_backend.h"

static float cv[160][240][3];
static uint8_t scr[MAX_SCRATCH][64 * 64]; static int scrW[MAX_SCRATCH], scrH[MAX_SCRATCH], scr8[MAX_SCRATCH];
int gHostFrame;
static int nframes, stopAt = 90; static char dumpList[256];
static float ui[448][640][3]; static void (*uiHook)(void);
void BeSetUiHook(void (*h)(void)) { uiHook = h; }
void BeSetVideo(int size, int perf) { (void)size; (void)perf; }
void BeUiRect(int x, int y, int w, int h, int r, int g, int b, int a) {
    for (int py = y * 2; py < (y + h) * 2; py++) for (int px = x * 2; px < (x + w) * 2; px++) {
        if (px < 0 || py < 0 || px >= 640 || py >= 448) continue;
        float c[3] = {r, g, b}; for (int k = 0; k < 3; k++) ui[py][px][k] = c[k] * a / 128.f + ui[py][px][k] * (1 - a / 128.f); }
}
static void DumpUi(const char *name) {
    FILE *f = fopen(name, "wb"); fprintf(f, "P6\n640 448\n255\n");
    for (int y = 0; y < 448; y++) for (int x = 0; x < 640; x++) for (int k = 0; k < 3; k++) fputc((int)ui[y][x][k], f);
    fclose(f);
}
static inline int Swz(int i) { return (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1); }

void BeInit(void) { if (getenv("AC")) {
    struct sigaction a; memset(&a, 0, sizeof a); a.sa_flags = SA_SIGINFO | SA_NODEFER; a.sa_sigaction = OnBus; sigaction(SIGBUS, &a, 0);
    a.sa_sigaction = OnTrap; sigaction(SIGTRAP, &a, 0);
    __asm__ volatile("pushf; orl $0x40000,(%esp); popf"); }
  if (getenv("STOP")) stopAt = atoi(getenv("STOP")); if (getenv("DUMP")) snprintf(dumpList, sizeof dumpList, ",%s,", getenv("DUMP")); }
void BeBeginFrame(uint16_t b) {
    for (int y = 0; y < 160; y++) for (int x = 0; x < 240; x++) {
        cv[y][x][0] = (b & 31) * 255 / 31; cv[y][x][1] = ((b >> 5) & 31) * 255 / 31; cv[y][x][2] = ((b >> 10) & 31) * 255 / 31; }
}
void BeSync(void) {}
void BeScratch(int s, int is8, int w, int h, const uint8_t *d) { memcpy(scr[s], d, w * h / (is8 ? 1 : 2)); scrW[s] = w; scrH[s] = h; scr8[s] = is8; }

static int Sample(int id, int clut, int u, int v) { /* retorna rgb555|0x8000 ou 0 */
    if (u < 0 || v < 0) return 0;
    if (id < 4) { if (u >= 512 || v >= 512) return 0; int i = gBgLayer[id].pix[v * 512 + u]; return clut == CLUT_BG8 ? gClutBg8[Swz(i)] : gClutBg4[Swz(i)]; }
    if (id == TEX_OBJ4) { if (u >= 256 || v >= 256) return 0; uint8_t b = gObjAtlas4[v * 128 + u / 2]; int c = (u & 1) ? b >> 4 : b & 15; return gClutObj4[clut - 3][c]; }
    if (id == TEX_OBJ8) { if (u >= 128 || v >= 256) return 0; return gClutObj8[Swz(gObjAtlas8[v * 128 + u])]; }
    int s = id - TEX_SCRATCH0; if (u >= scrW[s] || v >= scrH[s]) return 0;
    if (scr8[s]) return gClutObj8[Swz(scr[s][v * scrW[s] + u])];
    uint8_t b = scr[s][v * (scrW[s] / 2) + u / 2]; int c = (u & 1) ? b >> 4 : b & 15; return gClutObj4[clut - 3][c];
}
static void Put(int x, int y, int col, int alpha, int shade) {
    if (x < 0 || y < 0 || x >= 240 || y >= 160 || !(col & 0x8000)) return;
    float c[3] = {(col & 31) * 255 / 31.f, ((col >> 5) & 31) * 255 / 31.f, ((col >> 10) & 31) * 255 / 31.f};
    for (int k = 0; k < 3; k++) { float s = c[k] * shade / 128.f; cv[y][x][k] = s * alpha / 128.f + cv[y][x][k] * (1 - alpha / 128.f); }
}
void BeQuad(int id, int clut, float x, float y, float w, float h, float u0, float v0, float u1, float v1, int alpha, int shade) {
    for (int py = (int)lroundf(y); py < (int)lroundf(y + h); py++) for (int px = (int)lroundf(x); px < (int)lroundf(x + w); px++) {
        float tu = u0 + (px + .5f - x) / w * (u1 - u0), tv = v0 + (py + .5f - y) / h * (v1 - v0);
        Put(px, py, Sample(id, clut, (int)floorf(tu), (int)floorf(tv)), alpha, shade); }
}
void BeQuadAffine(int id, int clut, const float *x, const float *y, const float *u, const float *v, int alpha, int shade) {
    float ex = x[1] - x[0], ey = y[1] - y[0], fx = x[2] - x[0], fy = y[2] - y[0], det = ex * fy - ey * fx;
    if (fabsf(det) < 1e-6f) return;
    float mnx = fminf(fminf(x[0], x[1]), fminf(x[2], x[3])), mxx = fmaxf(fmaxf(x[0], x[1]), fmaxf(x[2], x[3]));
    float mny = fminf(fminf(y[0], y[1]), fminf(y[2], y[3])), mxy = fmaxf(fmaxf(y[0], y[1]), fmaxf(y[2], y[3]));
    for (int py = (int)floorf(mny); py <= (int)ceilf(mxy); py++) for (int px = (int)floorf(mnx); px <= (int)ceilf(mxx); px++) {
        float dx = px + .5f - x[0], dy = py + .5f - y[0];
        float a = (dx * fy - dy * fx) / det, b = (ex * dy - ey * dx) / det;
        if (a < 0 || a >= 1 || b < 0 || b >= 1) continue;
        Put(px, py, Sample(id, clut, (int)floorf(u[0] + a * (u[1] - u[0])), (int)floorf(v[0] + b * (v[2] - v[0]))), alpha, shade); }
}
void BeOverlay(int white, int alpha) { for (int y = 0; y < 160; y++) for (int x = 0; x < 240; x++) for (int k = 0; k < 3; k++) cv[y][x][k] = (white ? 255 : 0) * alpha / 128.f + cv[y][x][k] * (1 - alpha / 128.f); }
static void Dump(const char *name) {
    FILE *f = fopen(name, "wb"); fprintf(f, "P6\n240 160\n255\n");
    for (int y = 0; y < 160; y++) for (int x = 0; x < 240; x++) for (int k = 0; k < 3; k++) fputc((int)cv[y][x][k], f);
    fclose(f);
}
void BeEndFrame(void) {
    ++nframes; gHostFrame = nframes;
    if (uiHook) {
        for (int y = 0; y < 448; y++) for (int x = 0; x < 640; x++) { int gx = (x - 20) * 240 / 600, gy = (y - 24) * 160 / 400; for (int k = 0; k < 3; k++) ui[y][x][k] = (gx >= 0 && gx < 240 && gy >= 0 && gy < 160) ? cv[gy][gx][k] : 0; }
        uiHook();
        char key2[16], n2[32]; snprintf(key2, sizeof key2, ",%d,", nframes);
        if (dumpList[0] && strstr(dumpList, key2)) { snprintf(n2, sizeof n2, "ui_%d.ppm", nframes); DumpUi(n2); }
    }
    char key[16], name[32]; snprintf(key, sizeof key, ",%d,", nframes);
    if (dumpList[0] && strstr(dumpList, key)) { snprintf(name, sizeof name, "frame_%d.ppm", nframes); Dump(name);
        extern unsigned char gGbaIo[]; unsigned short *io = (unsigned short *)gGbaIo;
        printf("frame %d DISPCNT=%04x BG0..3CNT=%04x %04x %04x %04x WININ=%04x WINOUT=%04x BLDCNT=%04x BLDALPHA=%04x BLDY=%04x WIN0H=%04x WIN0V=%04x\n", nframes, io[0], io[4], io[5], io[6], io[7], io[0x24], io[0x25], io[0x28], io[0x29], io[0x2a], io[0x20], io[0x22]);
        { extern unsigned char gMainGameMode, gDisablePause; extern short gSubGameMode1; extern signed char gSubGameMode2; extern unsigned short gButtonInput, gChangedInput; printf("mode=%d sub1=%d sub2=%d dispause=%d btn=%04x chg=%04x key=%04x\n", gMainGameMode, gSubGameMode1, gSubGameMode2, gDisablePause, gButtonInput, gChangedInput, ((unsigned short*)gGbaIo)[0x98]); }
        printf("WIN1H=%04x WIN1V=%04x\n", io[0x21], io[0x23]);
        extern unsigned char gGbaOam[]; unsigned short *o = (unsigned short *)gGbaOam;
        for (int i = 0; i < 128; i++) { unsigned a0 = o[i*4], a1 = o[i*4+1], a2 = o[i*4+2]; int x = a1 & 0x1ff, y = a0 & 0xff; if (((a0>>8)&3)==2) continue; if (x >= 190 && x < 256 && y < 48) printf("obj%d a0=%04x a1=%04x a2=%04x x=%d y=%d\n", i, a0, a1, a2, x, y); } }
    if (nframes == stopAt) { if (!dumpList[0]) Dump("frame.ppm"); exit(0); }
}
uint32_t BeCycles(void) { return 0; }
void BeProfileMark(int s, uint32_t c) { (void)s; (void)c; }
