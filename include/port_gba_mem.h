/* Camada minima de enderecos GBA <-> host para o motor de audio (PS2: ponteiros de 32 bits, mesma largura do GBA). */
#ifndef PORT_GBA_MEM_H
#define PORT_GBA_MEM_H
#include <stdint.h>
#include <stddef.h>
extern unsigned char gGbaIo[];
#define gIoMem gGbaIo
/* Resolve um endereco bruto da ROM do GBA (0x08xxxxxx) para o ponteiro dentro da ROM carregada em RAM; o resto passa direto. */
void *port_resolve_addr(uintptr_t val);
#define GBA_RESOLVE(p) ((__typeof__(p))port_resolve_addr((uintptr_t)(p)))
#endif
