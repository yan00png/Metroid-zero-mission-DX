#ifndef GFX_CACHE_H
#define GFX_CACHE_H
#include <stdint.h>

/* Camada de BG pre-composta: indices 8 bits (4bpp: banco*16+cor), stride fixo de 512. */
typedef struct {
    uint8_t *pix;
    int wPx, hPx, is8bpp, enabled, affine; /* affine: BG rotativo (modos 1/2), mapa de 1 byte por tile */
    int dirty;              /* precisa reenviar ao GS */
    uint32_t sig;           /* assinatura do BGxCNT: detecta troca de config */
    uint16_t shadowMap[64 * 64];
    uint8_t empty[64 * 64]; /* tile todo transparente (indice 0): o desenho pula essas areas (economiza fill do GS) */
} BgLayerCache;

#define LAYER_STRIDE 512

extern BgLayerCache gBgLayer[4];
extern int gPalDirty, gObjDirty;
/* CLUTs prontas (bit15=opaco, indice 0 transparente). As de 256 ja estao no layout CSM1 do GS. */
extern uint16_t gClutBg4[256], gClutBg8[256], gClutObj8[256], gClutObj4[16][16];
extern uint8_t gObjAtlas4[], gObjAtlas8[]; /* 256x256 T4 e 128x256 T8 (32KB cada) */

void GfxCacheInit(void);
void GfxCacheUpdate(void);
/* Monta um sprite wT x hT tiles em memoria linear (para sprites afins). Retorna bytes. */
int GfxAssembleObj(int is8, int oneD, int base, int wT, int hT, uint8_t *out);
#endif
