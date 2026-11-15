#ifndef GFX_BACKEND_H
#define GFX_BACKEND_H
#include <stdint.h>
#include <stddef.h>
/* Ids de textura: 0-3 camadas de BG; 4 atlas OBJ 4bpp; 5 atlas OBJ 8bpp; 6.. scratch (sprites afins).
 * Ids de CLUT: 0 BG4; 1 BG8; 2 OBJ8; 3+banco OBJ4. Coordenadas em pixels GBA (240x160).
 * alpha/shade: 0..128 (128 = opaco / sem escurecer). */
#define TEX_OBJ4 4
#define TEX_OBJ8 5
#define TEX_SCRATCH0 6
#define MAX_SCRATCH 32
#define CLUT_BG4 0
#define CLUT_BG8 1
#define CLUT_OBJ8 2
#define CLUT_OBJ4(b) (3 + (b))

void BeInit(void);
void BeBeginFrame(uint16_t backdrop555);
void BeBackdropRows(int y0, int y1, uint16_t backdrop555); /* cor de fundo por faixa de linhas (gradiente via DMA de HBlank) */
int PlatformHblankValue(uintptr_t addr, int y, uint16_t *out); /* 1 = um DMA de HBlank cobre esse endereco do GBA */
void BeSync(void); /* envia ao GS o que o cache marcou como alterado */
void BeQuad(int tex, int clut, float x, float y, float w, float h, float u0, float v0, float u1, float v1, int alpha, int shade);
/* vertices na ordem TL,TR,BL,BR da textura */
void BeQuadAffine(int tex, int clut, const float *x, const float *y, const float *u, const float *v, int alpha, int shade);
int BeFilterLinear(void);   /* 1 = bilinear ligado */
int BeScratchOk(void);      /* 0 se os scratch estao desligados no debug */
void BeScratch(int slot, int is8, int wPx, int hPx, const uint8_t *data);
void BeOverlay(int white, int alpha);
void BeBlendAdd(int on);
void BeOverlayRect(float x, float y, float w, float h, int white, int alpha);
void BeEndFrame(void);
void RenderFrame(void);
/* menu/UI: retangulo em coordenadas virtuais 320x224, cor 0..255, alpha 0..128; hook chamado no fim do quadro */
void BeUiRect(int x, int y, int w, int h, int r, int g, int b, int alpha);
void BeSetUiHook(void (*hook)(void));
/* video: size 0=2.0x 1=2.5x 2=ajustar a tela; perfBars liga as barras de desempenho */
void BeSetVideo(int size, int perfBars);
void BeShutdown(void);   /* desmonta o video antes de abrir outro ELF */
void BeSetWsMask(int on);   /* 1 = esta cena nao usa as colunas extras: as laterais do 16:9 ficam pretas */
int BeWsPad(void);   /* colunas extras de cada lado no 16:9 nativo (0 = 240 normal) */
void BeSetColorFx(int on);  /* correcao de cor do GBA nas paletas */
void BeSetScreenFx(int fx); /* 0 = nada, 1 = grade LCD, 2 = scanlines (480i/480p/1080i) */
void BeSetVideoMode(int mode, int aspect, int filter); /* 0=160p 1=480i 2=480p 3=1080i; aspecto 0=16:9 1=4:3 2=3:2 (480p/1080i); filtro 0=nitido 1=bilinear */
/* perfil: ciclos do EE (294.9 MHz); marcos por frame desenhados como barras */
void BeGameToUi(float gx, float gy, int *ux, int *uy);
uint32_t BeCycles(void);
void BeProfileMark(int slot, uint32_t cycles);
void BeProfileFreeze(int f);
float BeProfileMs(int slot);
float BeProfilePeriodMs(void);
void BeProfileMark(int slot, uint32_t cycles);
void BeNesPresent(const uint16_t *pix, int overscan, int aspect, int linear, int scan, void (*overlay)(void));
void BeNesDone(void);
#endif
