#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rom.h"
#include "rom_ref.h"
#include "storage.h"

extern void MenuStatus(const char *);
extern void PlatformBeat(void);
/* tabela gerada por tools/romsplit.py: cada entrada = (destino em RAM, offset na ROM, tamanho) */
struct RomEntry { uint8_t *dst; uint32_t off; uint32_t size; };
extern struct RomEntry __start_romtab[], __stop_romtab[];

#define CHUNK (16 * 1024)
extern uint8_t gRomData[];   /* audio_ps2.c: a ROM inteira fica em RAM (o motor de som le por enderecos da ROM) */
extern void AudioPs2_OnRomLoaded(void);
static char sFound[48];
static int sFoundSize;
static int sErrPos, sErrRet; static uint32_t sErrFilePos;

static uint32_t sCrcTab[256];
static void CrcInit(void)
{
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; sCrcTab[i] = c; }
}
static uint32_t Crc(uint32_t c, const uint8_t *p, int n)
{
    while (n--) c = sCrcTab[(c ^ *p++) & 255] ^ (c >> 8);
    return c;
}

static int ReadFull(int fd, void *dst, int n)
{
    uint8_t *d = dst; int got = 0;
    while (got < n) { int r = StorageRead(fd, d + got, n - got); if (r <= 0) { sErrPos = got; sErrRet = r; return -1; } got += r; }
    return got;
}

int RomLoad(void)
{
    if (!StorageReady()) return ROM_NO_USB;
    int size = StorageFileSize(ROM_FILE_NAME);
    if (size < 0) return ROM_NOT_FOUND;
    int fd = StorageOpen(ROM_FILE_NAME, 0);
    if (fd < 0) return ROM_NOT_FOUND;
    if (size < 0xC0) { StorageClose(fd); sFoundSize = size; return ROM_NOT_MZM; }

    /* cabecalho: titulo (0xA0), codigo do jogo (0xAC), versao (0xBC) */
    uint8_t *rom = gRomData;
    int first = CHUNK > size ? size : CHUNK;
    if (ReadFull(fd, rom, first) < 0) { sErrFilePos = 0; StorageClose(fd); return ROM_READ_ERROR; }
    char code[5]; memcpy(code, rom + 0xAC, 4); code[4] = 0;
    for (int i = 0; i < 4; i++) if (code[i] < 32 || code[i] > 126) code[i] = '?';
    snprintf(sFound, sizeof sFound, "%s", code); sFoundSize = size;
    if (memcmp(rom + 0xA0, "ZEROMISSION", 11)) { StorageClose(fd); return ROM_NOT_MZM; }
    if (code[3] != 'E') { StorageClose(fd); return ROM_OTHER_REGION; }
    if (rom[0xBC] != 0) { StorageClose(fd); return ROM_OTHER_VERSION; }
    if ((uint32_t)size != MZM_ROM_SIZE) { StorageClose(fd); return ROM_BAD_SIZE; }

    /* le o restante direto na RAM e confere o CRC32 do arquivo inteiro */
    CrcInit();
    uint32_t crc = Crc(0xFFFFFFFFu, rom, first);
    for (uint32_t pos = first; pos < MZM_ROM_SIZE; pos += CHUNK)
    {
        int n = (MZM_ROM_SIZE - pos) < CHUNK ? (int)(MZM_ROM_SIZE - pos) : CHUNK;
        if (ReadFull(fd, rom + pos, n) < 0) { sErrFilePos = pos; StorageClose(fd); return ROM_READ_ERROR; }
        crc = Crc(crc, rom + pos, n);
        PlatformBeat();
        if (!(pos & 0xFFFFF)) { static char m[48]; snprintf(m, sizeof m, "CARREGANDO ROM... %u%%", pos * 100u / MZM_ROM_SIZE); MenuStatus(m); }
    }
    StorageClose(fd);
    if ((crc ^ 0xFFFFFFFFu) != MZM_ROM_CRC32) return ROM_BAD_CRC;

    /* copia os trechos usados pelo jogo para seus simbolos */
    for (struct RomEntry *e = __start_romtab; e != __stop_romtab; e++) memcpy(e->dst, rom + e->off, e->size);
    AudioPs2_OnRomLoaded();
    return ROM_OK;
}

const char *RomErrorTitle(int c)
{
    switch (c)
    {
        case ROM_NO_USB: return "PEN-DRIVE NAO ENCONTRADO";
        case ROM_NOT_FOUND: return "ROM NAO ENCONTRADA";
        case ROM_READ_ERROR: return "ERRO AO LER A ROM";
        case ROM_BAD_SIZE: return "ROM COM TAMANHO INCORRETO";
        case ROM_NOT_MZM: return "ARQUIVO NAO E METROID ZERO MISSION";
        case ROM_OTHER_REGION: return "ROM DE OUTRA REGIAO";
        case ROM_OTHER_VERSION: return "ROM DE OUTRA VERSAO";
        case ROM_BAD_CRC: return "ROM INCOMPATIVEL (CHECKSUM)";
    }
    return "ERRO";
}

int RomErrorLines(int c, const char *l[4])
{
    static char a[64], b[64];
    switch (c)
    {
        case ROM_NO_USB:
            l[0] = StorageStatus(); l[1] = "ESPETE UM PEN-DRIVE FAT32 OU EXFAT"; l[2] = "COM O ARQUIVO " ROM_FILE_NAME; return 3;
        case ROM_NOT_FOUND:
            if (StorageIsDisc()) { l[0] = "A IMAGEM (ISO) NAO TEM A ROM"; l[1] = "REGERE A ISO COM tools/mkiso.sh"; l[2] = "(ARQUIVO MZM.GBA NA RAIZ)."; return 3; }
            l[0] = "COLOQUE " ROM_FILE_NAME " NA MESMA PASTA"; l[1] = "DO ELF, NO PEN-DRIVE."; snprintf(a, sizeof a, "PASTA: %s", StorageBaseDir()); l[2] = a; return 3;
        case ROM_READ_ERROR:
            l[0] = "FALHA AO LER O ARQUIVO. REGRAVE A ROM"; l[1] = StorageIsDisc() ? "OU CONFIRA A ISO/O DISCO." : "OU TROQUE O PEN-DRIVE.";
            snprintf(a, sizeof a, "DETALHE: BYTES=%d RET=%d POS=%u", sErrPos, sErrRet, sErrFilePos); l[2] = a; return 3;
        case ROM_BAD_SIZE:
            snprintf(a, sizeof a, "TAMANHO: %d BYTES (ESPERADO %u)", sFoundSize, MZM_ROM_SIZE); l[0] = a;
            l[1] = "USE A ROM ORIGINAL SEM MODIFICACOES."; return 2;
        case ROM_NOT_MZM:
            l[0] = "O ARQUIVO " ROM_FILE_NAME " NAO TEM"; l[1] = "O CABECALHO DE METROID ZERO MISSION."; return 2;
        case ROM_OTHER_REGION:
            snprintf(a, sizeof a, "CODIGO DA ROM: %s (PRECISA SER BMXE)", sFound); l[0] = a;
            l[1] = "ESTE PORT USA A ROM AMERICANA (USA)."; return 2;
        case ROM_OTHER_VERSION:
            l[0] = "ESTE PORT USA A REVISAO 0 DA ROM USA."; return 1;
        case ROM_BAD_CRC:
            l[0] = "A ROM FOI MODIFICADA, TRADUZIDA OU"; l[1] = "ESTA CORROMPIDA. USE A ROM USA ORIGINAL"; l[2] = "(SHA1 5DE8536A...)."; return 3;
    }
    return 0;
}
