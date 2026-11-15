/* Pecas do GBA ainda sem implementacao no PS2: audio (motor de som em assembly ARM)
 * e cabo de link. Retornam 0 e nao fazem nada. Serao trocadas fase a fase. */
#include <stdint.h>

#define STUB(name) int name() { return 0; }
STUB(IntrMain) STUB(SoundBias0) STUB(SoundBias200)
STUB(MultiBootInit) STUB(MultiBootMain) STUB(MultiBootCheckComplete) STUB(MultiBootStartParent)

/* "ROM de transferencia" do cabo de link (so tamanho simbolico) */
const uint8_t sTransferRom_After[1] = {0};

/* Substitui o codigo Thumb do haze (processava tabelas para DMA de HBlank). Devolve 1 = "efeito terminou". */
uint32_t PlatformHazeStub(void) { return 1; }
