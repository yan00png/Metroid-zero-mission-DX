#ifndef ROM_H
#define ROM_H
/* Carrega os assets do jogo a partir de metroid_zero.gba (ROM US original) na inicializacao. */
enum {
    ROM_OK = 0, ROM_NO_USB, ROM_NOT_FOUND, ROM_READ_ERROR, ROM_BAD_SIZE,
    ROM_NOT_MZM, ROM_OTHER_REGION, ROM_OTHER_VERSION, ROM_BAD_CRC
};
#define ROM_FILE_NAME "metroid_zero.gba"
int RomLoad(void);                    /* ROM_OK ou codigo de erro */
const char *RomErrorTitle(int code);
/* ate 4 linhas de detalhe, cada uma com <= 46 caracteres; retorna o numero de linhas */
int RomErrorLines(int code, const char *lines[4]);
#endif
