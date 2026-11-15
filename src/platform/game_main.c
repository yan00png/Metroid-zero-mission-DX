/* Ponto de entrada do port: liga plataforma e renderizador, carrega os assets da ROM e entrega o controle ao agbmain() do jogo. */
#include "platform.h"
#include "storage.h"
#include "rom.h"
#include "../gfx_cache.h"
#include "../gfx_backend.h"
#include "../menu.h"

extern void agbmain(void);

int main(int argc, char **argv)
{
    StorageResetIop(argc > 0 ? argv[0] : 0);
    GfxCacheInit();
    BeInit();
    PlatformInit();
    StorageInit(argc > 0 ? argv[0] : 0);
    if (!(PlatformReadPad() & 0x0C)) MenuVideoLoad();   /* segurando SELECT ou START ao ligar: ignora o video salvo (modo seguro) */

    for (;;)
    {
        MenuStatus("CARREGANDO " ROM_FILE_NAME "...");
        int code = RomLoad();
        if (code == ROM_OK) break;
        const char *lines[4];
        int n = RomErrorLines(code, lines);
        if (!MenuMessage(RomErrorTitle(code), lines, n, "X: TENTAR DE NOVO     O: SAIR")) PlatformExit();
        if (!StorageReady()) StorageMount(30);
    }
    extern void SaveLoad(void);
    SaveLoad();
    PlatformControlsLoad();
    extern int AudioPs2_Init(void);
    AudioPs2_Init();
    CrashInit();   /* so depois de carregar: a leitura da ROM pelo USB e lenta e nao bate o "coracao" do jogo */
    for (;;) agbmain(); /* agbmain retorna quando o jogo pede reset */
    return 0;
}
