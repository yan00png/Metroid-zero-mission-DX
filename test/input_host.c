/* Entrada de teste no host: KEYS="3@100-110,0@200-205" = segura a tecla (bit GBA) nos frames indicados. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "platform.h"
extern unsigned char gGbaIo[];
extern int gHostFrame;
void PlatformInit(void) { *(volatile uint16_t *)(gGbaIo + 0x130) = 0x3FF; }
uint16_t PlatformReadPad(void)
{
    uint16_t keys = 0;
    const char *e = getenv("KEYS");
    if (e) {
        char tmp[8192]; strncpy(tmp, e, 8191); tmp[8191] = 0;
        for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
            int k, a, b; if (sscanf(t, "%d@%d-%d", &k, &a, &b) == 3 && gHostFrame >= a && gHostFrame <= b) keys |= 1 << k;
        }
    }
    return keys;
}
void PlatformExit(void) { exit(0); }
