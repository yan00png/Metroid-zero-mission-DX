#define NEWLIB_PORT_AWARE
/* Drivers USB mass storage (FAT32 e exFAT) embutidos no ELF e carregados no IOP no boot. */
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <fileXio_rpc.h>
#include <sbv_patches.h>
#include <iopcontrol.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "storage.h"
#include "rom.h"
#include "rom_ref.h"
#include <fileio.h>
#include <libmc.h>
#include <libpad.h>
#include <audsrv.h>

#define IRX(n) extern unsigned char n##_irx[]; extern unsigned int size_##n##_irx;
IRX(iomanX) IRX(fileXio) IRX(usbd) IRX(bdm) IRX(bdmfs_fatfs) IRX(usbmass_bd)

static char sBase[128] = "mass0:/";
static char sStatus[48] = "USB: nao iniciado";
static const char *sDev = "mass0:";
static int sReady;
static const char *sArgv0;
int StorageMount(int tries);

extern void MenuStatus(const char *);
static int LoadMod(const char *name, void *buf, unsigned int size)
{
    { static char m[40]; snprintf(m, sizeof m, "CARREGANDO DRIVER %s", name); MenuStatus(m); }
    int ret = 0, id = SifExecModuleBuffer(buf, size, 0, NULL, &ret);
    if (id < 0 || ret == 1) { snprintf(sStatus, sizeof sStatus, "USB: falha %s (%d)", name, id < 0 ? id : ret); return 0; }
    return 1;
}

static int DeviceUp(const char *dev)
{
    char p[16]; snprintf(p, sizeof p, "%s/", dev);
    int d = fileXioDopen(p);
    if (d < 0) return 0;
    fileXioDclose(d); return 1;
}


/* ---- Modo disco (ISO/CD/DVD): o ELF foi iniciado de "cdrom0:" (OPL, HDD, USB, SMB ou disco gravado).
 * A ROM e lida do disco (somente leitura, via fio da BIOS/OPL); saves e controles vao para o memory card
 * em mc?:/MZM/. Nao reinicia o IOP (isso derrubaria o CD virtual do OPL) e nao carrega drivers USB. ---- */
#define DISC_ROM_NAME "MZM.GBA"      /* nome ISO9660 8.3 da ROM dentro da imagem */
#define MC_DIR        "/MZM"
#define MC_SLOTS      2
#define MC_FILE_MAX   (32 * 1024)
#define MC_FD_BASE    1000
static int sDisc;
static int sMcPort = -1, sMcInit, sMcDead;
struct McFile { int used, write, len, pos; char name[48]; unsigned char *buf; };
static struct McFile sMc[MC_SLOTS];
static unsigned char sMcBuf[MC_SLOTS][MC_FILE_MAX] __attribute__((aligned(64)));

int StorageIsDisc(void) { return sDisc; }
const char *StorageUsbDev(void) { return (sReady && !sDisc) ? sDev : 0; }

/* ---- memory card via libmc (RPC do mcserv). Sincronia com timeout para nunca travar o jogo. ---- */
static int McWait(void)
{
    int cmd, r = -99;
    for (int i = 0; i < 3000; i++)           /* ~3 s */
    {
        int st = mcSync(1, &cmd, &r);        /* modo 1: nao bloqueia */
        if (st != 0) return r;               /* != 0: terminou */
        for (volatile int k = 0; k < 20000; k++) {}
    }
    return -99;                              /* timeout */
}

/* acha um memory card utilizavel (mc0, depois mc1); retorna a porta ou -1 */
static int McPort(void)
{
    if (sMcPort >= 0) return sMcPort;
    if (sMcDead) return -1;
    if (!sMcInit)
    {
        SifInitRpc(0);
        int a = SifLoadModule("rom0:XMCMAN", 0, NULL);
        int b = SifLoadModule("rom0:XMCSERV", 0, NULL);
        int c = mcInit(MC_TYPE_XMC);
        snprintf(sStatus, sizeof sStatus, "MC: mods %d %d init %d", a, b, c);
        sMcInit = 1;
        if (c < 0) { sMcDead = 1; return -1; }
    }
    int lastr = 0;
    for (int port = 0; port < 2; port++)
    {
        int type, freeb, fmt, r = -99;
        for (int k = 0; k < 2; k++)          /* a 1a consulta so registra a troca de cartao */
        {
            mcGetInfo(port, 0, &type, &freeb, &fmt);
            r = McWait();
        }
        lastr = r;
#ifdef SAVE_TEST
        if (r >= -1 && type == MC_TYPE_PS2 && !fmt) { mcFormat(port, 0); McWait(); mcGetInfo(port, 0, &type, &freeb, &fmt); r = McWait(); }
#endif
        if (r >= -1 && type == MC_TYPE_PS2 && !fmt) { lastr = -2; continue; }   /* nunca formata sozinho um cartao real */
        if (r >= -1 && type == MC_TYPE_PS2) { sMcPort = port; snprintf(sStatus, sizeof sStatus, "MC: OK mc%d", port); return port; }
    }
    snprintf(sStatus, sizeof sStatus, lastr == -2 ? "MC: CARTAO NAO FORMATADO" : "MC: sem cartao (%d)", lastr);
    sMcDead = 1;
    return -1;
}

static void McName(const char *rel, char *out, int n)
{
    const char *b = strrchr(rel, '/'); b = b ? b + 1 : rel;
    snprintf(out, n, MC_DIR "/%s", b);
}

static int McOpen(const char *rel, int write)
{
    int slot = -1;
    for (int i = 0; i < MC_SLOTS; i++) if (!sMc[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    int port = McPort(); if (port < 0) return -1;
    struct McFile *f = &sMc[slot];
    memset(f, 0, sizeof *f);
    f->buf = sMcBuf[slot]; f->write = write; McName(rel, f->name, sizeof f->name);
    if (!write)
    {
        mcOpen(port, 0, f->name, FIO_O_RDONLY);
        int fd = McWait(); if (fd < 0) return -1;
        mcRead(fd, f->buf, MC_FILE_MAX);
        int n = McWait();
        mcClose(fd); McWait();
        f->len = n > 0 ? n : 0;
    }
    f->used = 1;
    return MC_FD_BASE + slot;
}

static int McFlush(struct McFile *f)
{
    int port = McPort(); if (port < 0) return -1;
    mcMkDir(port, 0, MC_DIR); int rd = McWait();      /* ja existe -> erro ignorado */
    mcOpen(port, 0, f->name, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    int fd = McWait();
    if (fd < 0) { snprintf(sStatus, sizeof sStatus, "MC: erro abrir %d (dir %d)", fd, rd); return -1; }
    int ok = 1, w = 0;
    for (int pos = 0; pos < f->len && ok; )
    {
        int n = f->len - pos; if (n > 4096) n = 4096;
        mcWrite(fd, f->buf + pos, n);
        w = McWait();
        if (w != n) ok = 0; else pos += n;
    }
    mcClose(fd); McWait();
    snprintf(sStatus, sizeof sStatus, ok ? "MC: SALVO %d bytes" : "MC: erro gravar %d", ok ? f->len : w);
    return ok ? 0 : -1;
}

static int DiscInit(void)
{
    sDisc = 1; sReady = 1;
    SifInitRpc(0);
    fioInit();
    snprintf(sBase, sizeof sBase, "cdrom0:\\");
    snprintf(sStatus, sizeof sStatus, "DISCO: OK");
    return 1;
}
static int DiscOpenRom(void)
{
    char p[40]; snprintf(p, sizeof p, "cdrom0:\\%s;1", DISC_ROM_NAME);
    return fioOpen(p, FIO_O_RDONLY);
}

#ifdef STORAGE_HOST   /* so para teste no PCSX2: le de host:  */
#include <unistd.h>
int StorageInit(const char *argv0) { sReady = 1; snprintf(sBase, sizeof sBase, "host:"); snprintf(sStatus, sizeof sStatus, "USB: HOST"); return 1; }
int StorageMount(int t) { return 1; }
void StorageResetIop(const char *a) {}
int StorageOpen(const char *rel, int w) { char p[256]; snprintf(p, sizeof p, "%s%s", sBase, rel); return open(p, w ? (O_WRONLY|O_CREAT|O_TRUNC) : O_RDONLY, 0666); }
int StorageRead(int fd, void *b, int n) { return read(fd, b, n); }
int StorageWrite(int fd, const void *b, int n) { return write(fd, b, n); }
int StorageSeek(int fd, int o, int w) { return lseek(fd, o, w); }
void StorageClose(int fd) { close(fd); }
int StorageMkdir(const char *r) { return 0; }
int StorageFileSize(const char *rel) { int fd = StorageOpen(rel, 0); if (fd < 0) return -1; int n = lseek(fd, 0, SEEK_END); close(fd); return n; }
int StorageReady(void) { return 1; }
const char *StorageStatus(void) { return sStatus; }
const char *StorageBaseDir(void) { return sBase; }
#else
/* Reinicia o IOP (descarta drivers deixados por uLaunchELF/OPL/FMCB) para usar SO os modulos embutidos.
 * Nao reinicia quando o ELF vem de CD/DVD ou de host: (PCSX2/ps2link). Deve ser a primeira coisa do programa. */
void StorageResetIop(const char *argv0)
{
    if (argv0 && (!strncmp(argv0, "cdrom", 5) || !strncmp(argv0, "host", 4))) return;
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
}

int StorageInit(const char *argv0)
{
    if (argv0 && !strncmp(argv0, "cdrom", 5)) return DiscInit();
    SifInitRpc(0);
    sbv_patch_enable_lmb();   /* o LOADFILE da BIOS nao carrega modulos a partir de buffer sem este patch */
    if (!LoadMod("iomanX", iomanX_irx, size_iomanX_irx)) return 0;
    if (!LoadMod("fileXio", fileXio_irx, size_fileXio_irx)) return 0;
    if (fileXioInit() < 0) { snprintf(sStatus, sizeof sStatus, "USB: fileXioInit falhou"); return 0; }
    if (!LoadMod("usbd", usbd_irx, size_usbd_irx)) return 0;
    if (!LoadMod("bdm", bdm_irx, size_bdm_irx)) return 0;
    if (!LoadMod("fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx)) return 0;
    if (!LoadMod("usbmass", usbmass_bd_irx, size_usbmass_bd_irx)) return 0;
    sArgv0 = argv0;
    return StorageMount(40);
}

/* espera o pen-drive enumerar (tentativas de ~0,1 s); pode ser chamada de novo apos o usuario espetar o pen-drive */
int StorageMount(int tries)
{
    static const char *devs[] = { "mass0:", "mass:" };
    for (int t = 0; t < tries && !sReady; t++)
    {
        { static char m[40]; snprintf(m, sizeof m, "PROCURANDO PEN-DRIVE... %d", t / 10); if (t % 10 == 0) MenuStatus(m); }
        for (int i = 0; i < 2 && !sReady; i++)
            if (DeviceUp(devs[i])) { sDev = devs[i]; sReady = 1; }
        if (!sReady) { for (volatile int w = 0; w < 6000000; w++); }   /* ~0,1 s */
    }
    if (!sReady) { snprintf(sStatus, sizeof sStatus, "USB: pen-drive nao achado"); return 0; }

    /* pasta do ELF: se foi iniciado de "mass...:/dir/arquivo.elf", usa essa pasta */
    snprintf(sBase, sizeof sBase, "%s/", sDev);
    const char *argv0 = sArgv0;
    if (argv0 && !strncmp(argv0, "mass", 4))
    {
        const char *c = strchr(argv0, ':');
        const char *sl = strrchr(argv0, '/');
        if (c && sl && sl > c)
        {
            int n = (int)(sl - argv0) + 1;
            if (n < (int)sizeof sBase) { memcpy(sBase, argv0, n); sBase[n] = 0; }
        }
    }
    snprintf(sStatus, sizeof sStatus, "USB: OK %s", sDev);
    return 1;
}

int StorageReady(void) { return sReady; }
const char *StorageStatus(void) { return sStatus; }
const char *StorageBaseDir(void) { return sBase; }

static void Full(const char *rel, char *out, int n) { snprintf(out, n, "%s%s", sBase, rel); }

/* arquivos via POSIX do newlib (mesmo caminho que o Super Metroid PS2 usa; roteado para o fileXio) */
int StorageOpen(const char *rel, int write)
{
    if (sDisc)
    {
        if (!write && !strcmp(rel, ROM_FILE_NAME)) return DiscOpenRom();
        if (!write && !strcmp(rel, "metroid_zero.sav")) return fioOpen("cdrom0:\\MZM.SAV;1", FIO_O_RDONLY);   /* .sav na ISO (importa, so se nao houver save no memory card) */
        return McOpen(rel, write);
    }
    char p[256]; Full(rel, p, sizeof p);
    return open(p, write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0666);
}
int StorageRead(int fd, void *buf, int n)
{
    if (fd >= MC_FD_BASE)
    {
        struct McFile *f = &sMc[fd - MC_FD_BASE];
        int r = f->len - f->pos; if (r > n) r = n; if (r < 0) r = 0;
        memcpy(buf, f->buf + f->pos, r); f->pos += r; return r;
    }
    return sDisc ? fioRead(fd, buf, n) : read(fd, buf, n);
}
int StorageWrite(int fd, const void *buf, int n)
{
    if (fd >= MC_FD_BASE)
    {
        struct McFile *f = &sMc[fd - MC_FD_BASE];
        if (f->pos + n > MC_FILE_MAX) n = MC_FILE_MAX - f->pos;
        if (n <= 0) return 0;
        memcpy(f->buf + f->pos, buf, n); f->pos += n; if (f->pos > f->len) f->len = f->pos; return n;
    }
    return sDisc ? -1 : write(fd, buf, n);
}
int StorageSeek(int fd, int off, int whence)
{
    if (fd >= MC_FD_BASE)
    {
        struct McFile *f = &sMc[fd - MC_FD_BASE];
        int p = whence == 0 ? off : whence == 1 ? f->pos + off : f->len + off;
        if (p < 0) p = 0; if (p > MC_FILE_MAX) p = MC_FILE_MAX;
        f->pos = p; return p;
    }
    return sDisc ? fioLseek(fd, off, whence) : lseek(fd, off, whence);
}
void StorageClose(int fd)
{
    if (fd >= MC_FD_BASE)
    {
        struct McFile *f = &sMc[fd - MC_FD_BASE];
        if (f->write) McFlush(f);
        f->used = 0; return;
    }
    if (sDisc) fioClose(fd); else close(fd);
}
int StorageMkdir(const char *rel)
{
    if (sDisc) return 0;   /* a pasta /MZM do memory card e criada ao gravar */
    char p[256]; Full(rel, p, sizeof p); return mkdir(p, 0777);
}
int StorageFileSize(const char *rel)
{
    int fd = StorageOpen(rel, 0);
    if (fd < 0) return -1;
    int n = StorageSeek(fd, 0, SEEK_END);
    if (sDisc && fd < MC_FD_BASE && n <= 0) n = (int)MZM_ROM_SIZE;   /* lseek(END) pode nao existir no CD virtual: assume o tamanho esperado */
    StorageClose(fd);
    return n;
}
#endif
