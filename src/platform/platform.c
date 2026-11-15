/* Ciclo de frame do port: no GBA o jogo roda a logica e dorme (Halt) ate o VBlank;
 * a interrupcao de VBlank copia os dados para o hardware. Aqui, "dormir" = executar
 * o handler de VBlank do proprio jogo, desenhar o quadro no GS e ler o controle. */
#include "platform.h"
#include "menu.h"

extern unsigned char gGbaIo[];
extern void CallbackCallVblank(void);
extern void RenderFrame(void);

#include <stdio.h>
#include "../gfx_backend.h"
extern unsigned char gSubGameModeStage; extern short gMainGameMode, gSubGameMode1; extern signed char gSubGameMode2, gSubGameMode3;
extern unsigned short gFrameCounter16Bit, gButtonInput;

/* Linha de diagnostico mostrada no menu: onde o jogo esta (modo / sub-modos / estagio / contador de quadros). */
extern volatile int gPlatformStep;
void PlatformDebugLine(char *out, int n)
{
    snprintf(out, n, "M%d S%d/%d/%d T%d F%u P%d", (int)gMainGameMode, (int)gSubGameMode1, (int)gSubGameMode2,
             (int)gSubGameMode3, (int)gSubGameModeStage, (unsigned)gFrameCounter16Bit, gPlatformStep);
}

#ifdef MZM_TRACE
static void Trace(void)
{
    static int n;
    if (++n % 60) return;
    FILE *f = fopen("host:/tmp/ps2/trace.txt", "a");
    if (!f) return;
    fprintf(f, "f=%d main=%d s1=%d s2=%d s3=%d stage=%d fc=%u btn=%04x dispcnt=%04x\n", n, gMainGameMode, gSubGameMode1, gSubGameMode2, gSubGameMode3, gSubGameModeStage, gFrameCounter16Bit, gButtonInput, *(unsigned short *)gGbaIo);
    fclose(f);
}
#else
#define Trace()
#endif

extern void SaveTick(void);
void PlatformWaitVBlank(void)
{
    static uint32_t tExit;
    uint32_t tEnter = BeCycles();
    if (tExit) BeProfileMark(4, tEnter - tExit);     /* tempo na logica do jogo desde o quadro anterior */
    PlatformBeat();
    SaveTick();
#if CRASHTEST == 1
    { static int n; if (++n == 100) { ((void (*)(void))0x00100001)(); } }
#elif CRASHTEST == 2
    { static int n; if (++n == 100) for (;;); }
#endif
    Trace();
    CallbackCallVblank();   /* gVBlankCallback do jogo + gVBlankRequestFlag */
    BeProfileMark(5, BeCycles() - tEnter);
    RenderFrame();          /* o GS desenha o que o jogo deixou em VRAM/OAM/registradores */
    PlatformPollInput();    /* proximo UpdateInput() do jogo le REG_KEYINPUT */
    tExit = BeCycles();
}

/* Start+Select abre o menu. O Start chega ao jogo com 3 quadros de atraso para que, ao apertar os dois
 * quase juntos, o jogo nao abra a tela de pausa antes do menu. */
void PlatformPollInput(void)
{
    static uint16_t startHist;
    static int comboPrev;
#ifdef TEST_MENU
    { static int fr; if (++fr == 300) { MenuRun(); } }
#endif
#ifdef NES_AUTO
    { static int fr; if (++fr == 400) { NesRun(); return; } }
#endif
    uint16_t raw = PlatformReadPad();
    int combo = (raw & (1 << 3)) && (raw & (1 << 2));        /* Start+Select do controle (fixo, independe do remapeamento) */
    raw = PlatformMapPad(raw);
    startHist = (uint16_t)((startHist << 1) | ((raw >> 3) & 1));
    if (combo && !comboPrev)
    {
        comboPrev = 1;
        MenuRun();
        startHist = 0;
        *(volatile uint16_t *)(gGbaIo + 0x130) = 0x3FF;
        return;
    }
    comboPrev = combo;
    raw &= (uint16_t)~(1 << 3);
    raw |= (uint16_t)(((startHist >> 3) & 1) << 3);
    *(volatile uint16_t *)(gGbaIo + 0x130) = (uint16_t)(~raw & 0x3FF);
}

volatile int gPlatformStep;
void PlatformStep(int n) { gPlatformStep = n; }

void PlatformSyscall(int number)
{
    switch (number)
    {
        case 2: /* Halt: espera a proxima interrupcao (VBlank) */
        case 5: /* VBlankIntrWait */
            PlatformWaitVBlank();
            break;
        default: /* Stop (3), reset de RAM etc.: sem efeito por enquanto */
            break;
    }
}
