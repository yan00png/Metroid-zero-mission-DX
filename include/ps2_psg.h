#ifndef PS2_PSG_H
#define PS2_PSG_H
/* Sintetizador das 4 vozes PSG do GBA (2 quadrados, onda de 32 passos, ruido) para o port PS2.
 * O motor do jogo so escreve os registradores (0x04000060..0x0400009F, espelhados em gIoMem); no GBA o chip
 * gera o som. Aqui o sintetizador le esses registradores uma vez por quadro e gera as amostras diretamente na
 * taxa de saida (48 kHz), sem passar pela taxa baixa do mixer de amostras.
 * Escrito a partir do comportamento documentado do hardware (GBATek); nao deriva de outros ports. */
void Ps2Psg_Reset(void);
void Ps2Psg_Trigger(unsigned channel);               /* canal 1..4: nota nova (bit 15 do registrador de frequencia) */
void Ps2Psg_Frame(unsigned outRate);                 /* trava o estado dos registradores para o proximo bloco */
void Ps2Psg_Render(int *left, int *right);           /* gera uma amostra estereo (escala s16); 0 se tudo mudo */
int Ps2Psg_Active(void);                             /* 1 se alguma voz esta audivel neste bloco */
#endif
