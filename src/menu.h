#ifndef MENU_H
#define MENU_H
/* Menu do port (Start+Select): Resume game / Video / Exit game. O jogo fica parado enquanto o menu esta aberto. */
void MenuRun(void);
void MenuVideoLoad(void);   /* aplica o video salvo (save/video.cfg); chamar depois do StorageInit */
/* Aviso em tela cheia antes do jogo (ate 4 linhas). Retorna 1 se o usuario escolheu tentar de novo (X), 0 para sair (O). */
int MenuMessage(const char *title, const char **lines, int n, const char *foot);
void MenuStatus(const char *title);
int MenuLanguage(void);   /* ENGLISH / PORTUGUES (BR): define gPtBr */
extern int gPtBr;
#endif
