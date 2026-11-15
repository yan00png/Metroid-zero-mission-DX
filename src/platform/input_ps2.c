#define NEWLIB_PORT_AWARE
/* Controle do PS2 -> REG_KEYINPUT do GBA (libpad). */
#include <kernel.h>
#include <stdint.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <fileio.h>
#include <libpad.h>
#include "platform.h"
#include "storage.h"

extern unsigned char gGbaIo[];
static char padBuf[256] __attribute__((aligned(64)));
static int padReady;

void PlatformInit(void)
{
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    if (padPortOpen(0, 0, padBuf)) padReady = 1;
    *(volatile uint16_t *)(gGbaIo + 0x130) = 0x3FF; /* nenhuma tecla */
}

uint16_t PlatformReadPad(void)
{
    uint16_t keys = 0; /* 1 = pressionado */
    if (padReady)
    {
        int st = padGetState(0, 0);
        struct padButtonStatus b;
        if ((st == PAD_STATE_STABLE || st == PAD_STATE_FINDCTP1) && padRead(0, 0, &b))
        {
            uint16_t p = 0xFFFF ^ b.btns; /* libpad e ativo em 0 */
            if (p & PAD_CROSS)  keys |= 1 << 0;  /* A */
            if (p & PAD_SQUARE) keys |= 1 << 1;  /* B */
            if (p & PAD_SELECT) keys |= 1 << 2;
            if (p & PAD_START)  keys |= 1 << 3;
            if (p & PAD_RIGHT)  keys |= 1 << 4;
            if (p & PAD_LEFT)   keys |= 1 << 5;
            if (p & PAD_UP)     keys |= 1 << 6;
            if (p & PAD_DOWN)   keys |= 1 << 7;
            if (p & PAD_R1)     keys |= 1 << 8;
            if (p & PAD_L1)     keys |= 1 << 9;
            if (p & PAD_CIRCLE) keys |= 1 << 10;
            if (p & PAD_TRIANGLE) keys |= 1 << 11;
            if (p & PAD_L2)     keys |= 1 << 12;
            if (p & PAD_R2)     keys |= 1 << 13;
            if (p & PAD_R3)     keys |= 1 << 14;
        }
    }
    return keys;
}

void PlatformExit(void)
{
    Exit(0);
}

/* which: 0 = OPL (mc:/BOOT/BOOT.ELF), 1 = uLaunchELF (mc:/BOOT/BOOT2.ELF), 2 = browser da BIOS.
 * Retorna 0 se o ELF nao existe (o jogo continua); nao retorna se conseguiu lancar. */
int PlatformLaunch(int which)
{
    if (which == 2)
    {
        char *a[1] = { "BootBrowser" };
        ExecOSD(1, a);
    }
    static int mcLoaded;
    if (!mcLoaded)
    {
        SifInitRpc(0);
        SifLoadModule("rom0:XMCMAN", 0, NULL);
        SifLoadModule("rom0:XMCSERV", 0, NULL);
        fioInit();
        mcLoaded = 1;
    }
    static const char *paths[2][2] = { { "mc0:/BOOT/BOOT.ELF", "mc1:/BOOT/BOOT.ELF" },
                                       { "mc0:/BOOT/BOOT2.ELF", "mc1:/BOOT/BOOT2.ELF" } };
    for (int i = 0; i < 2; i++)
    {
        int fd = fioOpen(paths[which][i], FIO_O_RDONLY);
        if (fd < 0) continue;
        fioClose(fd);
        char *a[1] = { (char *)paths[which][i] };
        /* IOP limpo (sem os drivers do jogo), igual ao estado de boot, e so entao o EELOAD carrega o ELF */
        SifInitRpc(0);
        while (!SifIopReset("", 0)) {}
        while (!SifIopSync()) {}
        SifInitRpc(0);
        SifLoadModule("rom0:SIO2MAN", 0, NULL);
        SifLoadModule("rom0:MCMAN", 0, NULL);
        SifLoadModule("rom0:MCSERV", 0, NULL);
        a[0] = (char *)paths[which][i];
        LoadExecPS2(paths[which][i], 1, a);
    }
    return 0;
}

/* ---- remapeamento dos botoes: 6 botoes do GBA <- botoes do PS2 ---- */
const char *const gPs2BtnName[PS2_BTN_COUNT] = { "CROSS", "SQUARE", "CIRCLE", "TRIANGLE", "L1", "R1", "L2", "R2", "START", "SELECT" };
const char *const gGbaBtnName[GBA_BTN_COUNT] = { "A", "B", "L", "R", "START", "SELECT" };
static const uint8_t kRawBit[PS2_BTN_COUNT] = { 0, 1, 10, 11, 9, 8, 12, 13, 3, 2 };   /* bit de PlatformReadPad de cada botao do PS2 */
static const uint8_t kGbaBit[GBA_BTN_COUNT] = { 0, 1, 9, 8, 3, 2 };                  /* bit do KEYINPUT: A B L R START SELECT */
static const uint8_t kDefault[GBA_BTN_COUNT] = { 0, 1, 4, 5, 8, 9 };
uint8_t gPadMap[GBA_BTN_COUNT] = { 0, 1, 4, 5, 8, 9 };

/* bits do controle cru -> bits do KEYINPUT do GBA (direcional fixo, 6 botoes remapeaveis) */
uint16_t PlatformMapPad(uint16_t raw)
{
    uint16_t k = raw & 0xF0;
    for (int g = 0; g < GBA_BTN_COUNT; g++)
        if (raw & (1 << kRawBit[gPadMap[g]])) k |= (uint16_t)(1 << kGbaBit[g]);
    return k;
}

void PlatformControlsReset(void) { for (int i = 0; i < GBA_BTN_COUNT; i++) gPadMap[i] = kDefault[i]; }

#define CTRL_FILE "save/controls.cfg"
void PlatformControlsSave(void)
{
    uint8_t b[8] = { 'C', 'T', 'L', '1' };
    for (int i = 0; i < GBA_BTN_COUNT; i++) b[4 + i] = gPadMap[i];
    StorageMkdir("save");
    int fd = StorageOpen(CTRL_FILE, 1);
    if (fd < 0) return;
    StorageWrite(fd, b, 4 + GBA_BTN_COUNT);
    StorageClose(fd);
}

void PlatformControlsLoad(void)
{
    uint8_t b[16];
    int fd = StorageOpen(CTRL_FILE, 0);
    if (fd < 0) return;
    int n = StorageRead(fd, b, sizeof b);
    StorageClose(fd);
    if (n < 4 + GBA_BTN_COUNT || b[0] != 'C' || b[1] != 'T' || b[2] != 'L' || b[3] != '1') return;
    for (int i = 0; i < GBA_BTN_COUNT; i++) if (b[4 + i] < PS2_BTN_COUNT) gPadMap[i] = b[4 + i];
}
