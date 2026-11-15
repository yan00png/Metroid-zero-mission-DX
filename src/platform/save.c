/* Save (SRAM do GBA, 32 KB) em <pasta do ELF>/save/metroid_zero.sav */
#include <string.h>
#include "storage.h"

extern unsigned char gGbaSram[32 * 0x400];
#define SAVE_SIZE (32 * 0x400)
#define SAVE_DIR  "save"
#define SAVE_FILE "save/metroid_zero.sav"

static unsigned char sShadow[SAVE_SIZE];
static int sDirty, sQuiet;
static int sOk = 1;

void SaveLoad(void)
{
    static unsigned char tmp[SAVE_SIZE];
    int fd = StorageOpen(SAVE_FILE, 0);
    if (fd < 0) fd = StorageOpen("metroid_zero.sav", 0);   /* .sav de emulador na pasta do ELF (importa) */
    if (fd < 0) { memcpy(sShadow, gGbaSram, SAVE_SIZE); return; }
    int n = 0, r;
    while (n < SAVE_SIZE && (r = StorageRead(fd, tmp + n, SAVE_SIZE - n)) > 0) n += r;
    StorageClose(fd);
    if (n == SAVE_SIZE) memcpy(gGbaSram, tmp, SAVE_SIZE);
    memcpy(sShadow, gGbaSram, SAVE_SIZE);
}

static int SaveFlush(void)
{
    StorageMkdir(SAVE_DIR);
    int fd = StorageOpen(SAVE_FILE, 1);
    if (fd < 0) return 0;
    int n = 0, r;
    while (n < SAVE_SIZE && (r = StorageWrite(fd, gGbaSram + n, SAVE_SIZE - n)) > 0) n += r;
    StorageClose(fd);
    return n == SAVE_SIZE;
}

/* chamar 1x por frame: grava quando a SRAM mudou e ficou estavel por ~0,5 s */
void SaveTick(void)
{
    static int div;
#ifdef SAVE_TEST
    { static int t; if (++t == 600) gGbaSram[0x7000] ^= 1; }
#endif
    if (!sOk || (++div & 7)) return;
    if (memcmp(sShadow, gGbaSram, SAVE_SIZE)) { sDirty = 1; sQuiet = 0; memcpy(sShadow, gGbaSram, SAVE_SIZE); return; }
    if (sDirty && ++sQuiet >= 4) {
        sDirty = 0;
        if (!SaveFlush()) sDirty = 1, sQuiet = 0;   /* tenta de novo */
    }
}
