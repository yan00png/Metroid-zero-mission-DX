#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>
/* Camada de plataforma: substitui o que no GBA era hardware/BIOS (sem emular CPU). */
void PlatformInit(void);
void CrashInit(void);
void PlatformBeat(void);
void PlatformPollInput(void);      /* le o controle, filtra (Start+Select abre o menu) e escreve REG_KEYINPUT */
uint16_t PlatformReadPad(void);    /* controle cru (usado pelo menu): 1 = pressionado; bits 0 X, 1 Quadrado, 2 Select, 3 Start, 4-7 direcional,
                                    * 8 R1, 9 L1, 10 Circulo, 11 Triangulo, 12 L2, 13 R2 */
/* bit 14 do controle cru = R3 (liga/desliga o NES Metroid) */
void NesRun(void);
extern int gAudioVol[3], gAudioMono;   /* menu AUDIO: geral, DirectSound, PSG (0..100) e mono */
int AudioGain(int pct);
#define PS2_BTN_COUNT 10
#define GBA_BTN_COUNT 6
extern const char *const gPs2BtnName[PS2_BTN_COUNT], *const gGbaBtnName[GBA_BTN_COUNT];
extern uint8_t gPadMap[GBA_BTN_COUNT];      /* A, B, L, R, START, SELECT -> indice de gPs2BtnName */
uint16_t PlatformMapPad(uint16_t raw);      /* controle cru -> KEYINPUT do GBA (1 = pressionado) */
void PlatformControlsReset(void);
void PlatformControlsSave(void);
void PlatformControlsLoad(void);
void PlatformExit(void);
int PlatformLaunch(int which);
void PlatformDebugLine(char *out, int n); /* estado do jogo em texto, para o menu */           /* sai do jogo */
void PlatformWaitVBlank(void);     /* roda o handler de VBlank do jogo, desenha e le o controle */
void PlatformSyscall(int number);  /* trocou o 'svc N' do GBA */
#endif
