/* Menu do frontend, desenhado por cima do ultimo quadro do jogo com retangulos (fonte 5x7 propria,
 * sem depender de fonte da BIOS). Coordenadas virtuais 320x224. */
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include "menu.h"
#include "platform/storage.h"
#include "gfx_backend.h"
#include "platform.h"

/* bits do PlatformReadPad (iguais ao KEYINPUT do GBA) + circulo */
#define PB_CROSS 0
#define PB_SELECT 2
#define PB_START 3
#define PB_RIGHT 4
#define PB_LEFT 5
#define PB_UP 6
#define PB_DOWN 7
#define PB_CIRCLE 10

static const char *const sGlyph[][7] = {
    /* 0-9 */
    {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."},
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."},
    {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"},
    {"####.", "....#", "....#", ".###.", "....#", "....#", "####."},
    {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."},
    {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."},
    {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."},
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."},
    {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."},
    {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."},
    /* A-Z */
    {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."},
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."},
    {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."},
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},
    {"#####", "#....", "#....", "####.", "#....", "#....", "#...."},
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."},
    {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"},
    {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."},
    {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"},
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####"},
    {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"},
    {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"},
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."},
    {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"},
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"},
    {".####", "#....", "#....", ".###.", "....#", "....#", "####."},
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."},
    {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."},
    {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"},
    {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"},
    {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."},
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"},
    /* . + / : - < > */
    {".....", ".....", ".....", ".....", ".....", ".....", "..#.."},
    {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."},
    {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."},
    {".....", "..#..", ".....", ".....", "..#..", ".....", "....."},
    {".....", ".....", ".....", "#####", ".....", ".....", "....."},
    {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."},
    {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."},
    /* _ , ( ) ! ? */
    {".....", ".....", ".....", ".....", ".....", ".....", "#####"},
    {".....", ".....", ".....", ".....", "..##.", "..#..", ".#..."},
    {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."},
    {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."},
    {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."},
    {".###.", "#...#", "....#", "..##.", "..#..", ".....", "..#.."},
    {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"},   /* % */
};

static int GlyphIndex(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + c - 'A';
    switch (c)
    {
        case '.': return 36; case '+': return 37; case '/': return 38; case ':': return 39;
        case '-': return 40; case '<': return 41; case '>': return 42;
        case '_': return 43; case ',': return 44; case '(': return 45; case ')': return 46;
        case '!': return 47; case '?': return 48; case '%': return 49;
    }
    return -1;
}

static void Rect(int x, int y, int w, int h, uint32_t rgb, int a)
{
    BeUiRect(x, y, w, h, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
}

static int TextWidth(const char *s, int sc) { return (int)strlen(s) * 6 * sc; }
static int TextWidthH(const char *s, int sc2) { return (int)strlen(s) * 6 * sc2 / 2; }

/* sc2 = escala em meios: 2 = 1x, 3 = 1.5x, 4 = 2x */
static void TextH(int x, int y, int sc2, uint32_t rgb, const char *s)
{
    for (; *s; s++, x += 6 * sc2 / 2)
    {
        int g = GlyphIndex(*s);
        if (g < 0) continue;
        for (int r = 0; r < 7; r++)
        {
            const char *row = sGlyph[g][r];
            int y0 = y + r * sc2 / 2, h = (r + 1) * sc2 / 2 - r * sc2 / 2;
            for (int c = 0; c < 5;)
            {
                if (row[c] != '#') { c++; continue; }
                int e = c; while (e < 5 && row[e] == '#') e++;
                Rect(x + c * sc2 / 2, y0, e * sc2 / 2 - c * sc2 / 2, h, rgb, 128);
                c = e;
            }
        }
    }
}
static void Text(int x, int y, int sc, uint32_t rgb, const char *s) { TextH(x, y, sc * 2, rgb, s); }

/* ---- estado ---- */
enum { PAGE_MAIN, PAGE_VIDEO, PAGE_DEBUG, PAGE_CONTROL, PAGE_PERF, PAGE_AUDIO };
static int sPerfText, sFps;   /* tempos na tela durante o jogo */
static void Draw(void);
static void DrawPerfOverlay(void);
static void DrawOverlays(void);
extern int gBeDbg[5];
static int sPage, sSel, sNesStart;
static int sSize = 2;     /* 0 = 2.0x, 1 = 2.5x, 2 = ajustar a tela (padrao) */
static int sPerf = 0;     /* barras de desempenho */
static const char *const sSizeName[3] = {"2.0X", "2.5X", "FIT"};   /* 2.5X e FIT cintilam no 480i (linhas alternam entre 2 e 3) */
static int sVMode = 1;     /* 0 = 160P, 1 = 480I, 2 = 480P, 3 = 1080I */
static int sAspect = 0;    /* 480P/1080I: 0 = 16:9 nativo, 1 = 4:3, 2 = 3:2, 3 = 16:9 esticado */
static int sAspect480 = 1; /* 480I: 0 = 16:9 nativo, 1 = 4:3 (usa SCREEN SIZE) */
static int sColor = 0;     /* correcao de cor do GBA */
static int sFilter = 0;
/* efeito de tela: 0 = OFF, 1 = LCD, 2 = SCANLINE */
static const char *const sModeName[4] = {"160P", "480I", "480P", "1080I"};
static const char *const sAspectName[4] = {"16:9 WIDE", "4:3", "3:2", "16:9 STR"};
enum { V_MODE, V_SIZE, V_ASPECT, V_FILTER, V_COLOR, V_PERF, V_FPS, V_BACK };
static int sConfirm = 0, sConfirmLeft = 0, sPrevVMode = 1;   /* confirmacao do 1080i */

/* linhas da pagina de video conforme o modo */
static int VideoRows(int *ids)
{
    int n = 0;
    ids[n++] = V_MODE;
    if (sVMode >= 1) ids[n++] = V_ASPECT;
    if (sVMode == 1 && sAspect480 == 1) ids[n++] = V_SIZE;
    if (sVMode >= 1) ids[n++] = V_FILTER;   /* efeitos de tela: LCD / SCANLINE (o bilinear segue desativado) */
    ids[n++] = V_COLOR;
    ids[n++] = V_PERF; ids[n++] = V_FPS; ids[n++] = V_BACK;
    return n;
}

/* ---- visual no estilo da tela de status do jogo: moldura clara, aba magenta, barras cinza com icone laranja, barra preta de descricao ---- */
#define C_FRAME   0xC8D0D8
#define C_FRAME_D 0x606878
#define C_PANEL   0x1C1C2C
#define C_GRID    0x1E4040
#define C_BAR     0x4C4C5C
#define C_BAR_SEL 0x5A4A8C
#define C_ORANGE  0xE89020
#define C_TAB     0xA03CA0
#define C_LIME    0xD8E848
#define C_DIM     0x9098A8

static void Frame(int x, int y, int w, int h)
{
    Rect(x - 2, y - 2, w + 4, h + 4, C_FRAME, 128);          /* moldura clara */
    Rect(x - 1, y - 1, w + 2, h + 2, C_FRAME_D, 128);        /* sombra interna */
    Rect(x, y, w, h, C_PANEL, 124);
    for (int gx = 16; gx < w - 4; gx += 16) Rect(x + gx, y + 1, 1, h - 2, C_GRID, 90);   /* grade do fundo */
}

static void Tab(int cx, int y, const char *t)
{
    int w = TextWidthH(t, 3) + 20, x = cx - w / 2;
    Rect(x - 1, y - 1, w + 2, 20, C_FRAME, 128);
    Rect(x, y, w, 18, C_TAB, 128);
    TextH(x + 10, y + 4, 3, C_LIME, t);
}

static void Bar(int x, int y, int w, int h, int sel)
{
    Rect(x, y, w, h, sel ? C_BAR_SEL : C_BAR, 128);
    Rect(x, y, w, 1, sel ? 0xA090D8 : 0x9098A8, 128);        /* brilho em cima */
    Rect(x, y + h - 1, w, 1, 0x303040, 128);                 /* sombra embaixo */
    int by = y + (h - 7) / 2;
    Rect(x + 5, by, 7, 7, sel ? 0xFFE070 : C_ORANGE, 128);   /* icone laranja */
    Rect(x + 6, by + 1, 2, 2, 0xFFF0B0, 128);
}

static void DrawValue(int xr, int y, const char *v, uint32_t col, uint32_t arrow)
{
    if (v[0] == '<' && v[1] == ' ')
    {
        char in[16]; int n = (int)strlen(v) - 4; if (n < 0) n = 0; if (n > 15) n = 15;
        memcpy(in, v + 2, n); in[n] = 0;
        int x = xr - (n * 9 + 16);
        Text(x, y + 2, 1, arrow, "<");
        TextH(x + 8, y, 3, col, in);
        Text(x + 8 + n * 9 + 2, y + 2, 1, arrow, ">");
    }
    else TextH(xr - TextWidthH(v, 3), y, 3, col, v);
}

/* barra preta inferior: descricao da opcao (1 linha) + dica de teclas */
static void DescBar(int px, int y, int pw, const char *desc, const char *hint)
{
    Rect(px + 7, y - 1, pw - 14, 21, C_FRAME, 128);
    Rect(px + 8, y, pw - 16, 19, 0x000000, 128);
    Text(px + 12, y + 3, 1, 0xFFFFFF, desc);
    Text(px + 12, y + 11, 1, C_DIM, hint);
}

static void DrawConfirm(void)
{
    const int pw = 260, ph = 90, px = (320 - pw) / 2, py = (224 - ph) / 2;
    Rect(0, 0, 320, 224, 0x000000, 90);
    Frame(px, py, pw, ph);
    char line[40];
    snprintf(line, sizeof line, "KEEP %s?", sModeName[sVMode]);
    Tab(px + pw / 2, py + 8, line);
    const char *l2 = "X: KEEP   ANY OTHER: UNDO";
    Text(px + (pw - TextWidth(l2, 1)) / 2, py + 36, 1, 0xFFFFFF, l2);
    snprintf(line, sizeof line, "REVERTS IN %d", (sConfirmLeft + 59) / 60);
    Text(px + (pw - TextWidth(line, 1)) / 2, py + 52, 1, C_LIME, line);
    const char *l4 = "(480P/1080I NEED A COMPONENT CABLE)";
    Text(px + (pw - TextWidth(l4, 1)) / 2, py + 68, 1, C_DIM, l4);
}

/* descricao curta da opcao selecionada (sem acentos: a fonte so tem A-Z, 0-9 e sinais) */
static const char *Desc(const char *labels[], int sel, char *buf, int bn)
{
    if (sPage == PAGE_MAIN)
    {
        static const char *const d[7] = { "VOLTA PARA O JOGO", "MODO, TAMANHO, ASPECTO E FILTRO", "VOLUMES E SAIDA DO SOM", "ATRIBUI OS BOTOES DO CONTROLE",
                                          "TESTES DO RENDERIZADOR E TEMPOS", "METROID ORIGINAL (NES, 60 FPS)", "ABRE UM ELF DO MC OU DO USB" };
        return d[sel];
    }
    if (sPage == PAGE_AUDIO)
    {
        static const char *const d[5] = { "VOLUME DE TUDO (JOGO E NES)", "MUSICA E VOZES POR AMOSTRAS", "CANAIS DE ONDA (EFEITOS E BASE)", "ESTEREO OU MONO", "VOLTA AO MENU" };
        return d[sel];
    }
    if (sPage == PAGE_CONTROL)
    {
        if (sel < GBA_BTN_COUNT) { snprintf(buf, bn, "ESQ/DIR: BOTAO DO PS2 PARA O %s", gGbaBtnName[sel]); return buf; }
        return sel == GBA_BTN_COUNT ? "RESTAURA OS BOTOES PADRAO" : "SALVA E VOLTA AO MENU";
    }
    if (sPage == PAGE_DEBUG)
    {
        static const char *const d[8] = { "LIGA/DESLIGA AS CAMADAS DE FUNDO", "LIGA/DESLIGA OS SPRITES", "LIGA/DESLIGA OS SPRITES AFINS",
                                          "LIGA/DESLIGA FADES E EFEITOS DE COR", "OFF: NAO ENVIA TEXTURAS AO GS (TESTE)",
                                          "TEMPOS POR PARTE DO QUADRO", "MOSTRA OS TEMPOS SOBRE O JOGO", "VOLTA AO MENU" };
        return d[sel];
    }
    if (sPage == PAGE_PERF) return sel == 0 ? "VOLTA AO DEBUG" : "";
    /* video */
    int ids[8]; VideoRows(ids);
    switch (ids[sel])
    {
        case V_MODE: return "ALTERA A RESOLUCAO (480P+: COMPONENTE)";
        case V_SIZE: return "TAMANHO DA IMAGEM NO 480I";
        case V_ASPECT: return "PROPORCAO DA IMAGEM (480P/1080I)";
        case V_FILTER: return "EFEITO DE TELA: LCD OU SCANLINES";
        case V_COLOR: return "CORES COMO NA TELA DO GBA (MENOS SATURADAS)";
        case V_PERF: return "BARRAS DE DESEMPENHO NA TELA";
        case V_FPS: return "MOSTRA OS FPS ABAIXO DA ENERGIA";
        default: return "VOLTA AO MENU";
    }
}

static void Draw(void)
{
    if (sConfirm) { DrawConfirm(); return; }
    Rect(0, 0, 320, 224, 0x000000, 70);                 /* escurece o jogo */
    /* janela e posicao: iguais as da versao anterior */
    const int pw = 260, ph = sPage == PAGE_CONTROL ? 202 : 176, px = (320 - pw) / 2, py = (224 - ph) / 2;
    Frame(px, py, pw, ph);
    const char *title = sPage == PAGE_PERF ? "PERF (MS)" : sPage == PAGE_MAIN ? "MENU" : sPage == PAGE_VIDEO ? "VIDEO" : sPage == PAGE_AUDIO ? "AUDIO" : sPage == PAGE_CONTROL ? "CONTROLS" : "DEBUG GS";
    Tab(px + pw / 2, py + 5, title);
    { int tw = TextWidthH(title, 3) + 20, tx = px + pw / 2 - tw / 2;      /* tracos ao lado da aba */
      Rect(px + 8, py + 13, tx - px - 14, 2, C_FRAME, 128); Rect(tx + tw + 6, py + 13, px + pw - 8 - (tx + tw + 6), 2, C_FRAME, 128); }

    const char *labels[8]; char vals[8][20];
    int n = 3;
    if (sPage == PAGE_MAIN)
    {
        n = 6;
        labels[0] = "RESUME GAME"; labels[1] = "VIDEO"; labels[2] = "AUDIO"; labels[3] = "CONTROLS"; labels[4] = "DEBUG GS"; labels[5] = "METROID";
        for (int i = 0; i < 6; i++) vals[i][0] = 0;
    }
    else if (sPage == PAGE_AUDIO)
    {
        n = 5;
        labels[0] = "MASTER VOLUME"; labels[1] = "DIRECT SOUND"; labels[2] = "PSG CHANNELS"; labels[3] = "OUTPUT"; labels[4] = "BACK";
        for (int i = 0; i < 3; i++) snprintf(vals[i], 20, "< %d%% >", gAudioVol[i]);
        strcpy(vals[3], gAudioMono ? "< MONO >" : "< STEREO >"); vals[4][0] = 0;
    }
    else if (sPage == PAGE_CONTROL)
    {
        n = GBA_BTN_COUNT + 2;
        for (int i = 0; i < GBA_BTN_COUNT; i++)
        {
            static char lab[GBA_BTN_COUNT][12];
            snprintf(lab[i], sizeof lab[i], "%s", gGbaBtnName[i]);
            labels[i] = lab[i];
            snprintf(vals[i], 14, "< %s >", gPs2BtnName[gPadMap[i]]);
        }
        labels[GBA_BTN_COUNT] = "RESET DEFAULT"; labels[GBA_BTN_COUNT + 1] = "BACK";
        vals[GBA_BTN_COUNT][0] = vals[GBA_BTN_COUNT + 1][0] = 0;
    }
    else if (sPage == PAGE_DEBUG)
    {
        n = 8;
        labels[0] = "BG"; labels[1] = "OBJ"; labels[2] = "AFFINE"; labels[3] = "OVERLAY"; labels[4] = "UPLOADS";
        labels[5] = "PERF (MS)"; labels[6] = "PERF TEXT"; labels[7] = "BACK";
        for (int i = 0; i < 5; i++) strcpy(vals[i], gBeDbg[i] ? "< ON >" : "< OFF >");
        vals[5][0] = 0; strcpy(vals[6], sPerfText ? "< ON >" : "< OFF >"); vals[7][0] = 0;
    }
    else if (sPage == PAGE_PERF)
    {
        n = 1; labels[0] = "BACK"; vals[0][0] = 0;
        char l[48];
        static const char *nm[6] = { "GAME LOGIC", "SYSTEM", "LAYER CACHE", "TEXTURE UPLOAD", "DRAW LIST", "GS + VSYNC" };
        static const int slot[6] = { 4, 5, 0, 1, 2, 3 };
        float busy = 0;
        for (int i = 0; i < 6; i++)
        {
            float ms = BeProfileMs(slot[i]); if (slot[i] != 3) busy += ms;
            int t = (int)(ms * 100 + 0.5f);
            snprintf(l, sizeof l, "%-15s %2d.%02d", nm[i], t / 100, t % 100);
            Text(px + 14, py + 58 + i * 12, 1, i == 5 ? C_DIM : 0xFFFFFF, l);
        }
        float per = BeProfilePeriodMs();
        int tb = (int)(busy * 100 + 0.5f), tp = (int)(per * 100 + 0.5f), fps = per > 0.1f ? (int)(1000.0f / per + 0.5f) : 0;
        snprintf(l, sizeof l, "%-15s %2d.%02d / 16.67", "CPU BUSY", tb / 100, tb % 100);
        Text(px + 14, py + 58 + 6 * 12 + 4, 1, busy > 16.0f ? 0xFF7070 : 0x70FF90, l);
        snprintf(l, sizeof l, "%-15s %2d.%02d  %d FPS", "FRAME", tp / 100, tp % 100, fps);
        Text(px + 14, py + 58 + 7 * 12 + 4, 1, C_LIME, l);
    }
    else
    {
        int ids[8]; n = VideoRows(ids);
        for (int i = 0; i < n; i++)
        {
            vals[i][0] = 0;
            switch (ids[i])
            {
                case V_MODE: labels[i] = "VIDEO MODE"; snprintf(vals[i], 12, "< %s >", sModeName[sVMode]); break;
                case V_SIZE: labels[i] = "SCREEN SIZE"; snprintf(vals[i], 12, "< %s >", sSizeName[sSize]); break;
                case V_ASPECT: labels[i] = "ASPECT"; snprintf(vals[i], 14, "< %s >", sAspectName[sVMode == 1 ? sAspect480 : sAspect]); break;
                case V_COLOR: labels[i] = "COLOR"; strcpy(vals[i], sColor ? "< GBA >" : "< OFF >"); break;
                case V_FILTER: labels[i] = "FILTER"; { static const char *const fn[3] = { "< OFF >", "< LCD >", "< SCANLINE >" }; strcpy(vals[i], fn[sFilter]); } break;
                case V_FPS: labels[i] = "FPS MONITOR"; strcpy(vals[i], sFps ? "< ON >" : "< OFF >"); break;
                case V_PERF: labels[i] = "PERF BARS"; strcpy(vals[i], sPerf ? "< ON >" : "< OFF >"); break;
                default: labels[i] = "BACK"; break;
            }
        }
    }
    const int ry = py + 27, pitch = n >= 8 && sPage != PAGE_CONTROL ? 16 : n >= 7 ? 17 : 18;
    for (int i = 0; i < n; i++)
    {
        int y = ry + i * pitch, on = i == sSel; (void)on;
        Bar(px + 8, y, pw - 16, pitch >= 18 ? 16 : 15, i == sSel);
        TextH(px + 26, y + 3, 3, 0xF0F0F0, labels[i]);
        if (vals[i][0]) DrawValue(px + pw - 14, y + 3, vals[i], i == sSel ? C_LIME : 0xD0D4E0, i == sSel ? 0xFFFFFF : C_DIM);
    }
    char dbuf[48];
    const char *hint = sPage == PAGE_CONTROL ? "D-PAD E START+SELECT SAO FIXOS"
                     : (sPage == PAGE_VIDEO || sPage == PAGE_DEBUG || sPage == PAGE_AUDIO) ? "ESQ/DIR OU X: ALTERAR   O: VOLTAR"
                     : "CIMA/BAIXO: MOVER   X: OK   O: VOLTAR";
    DescBar(px, py + ph - 21, pw, sPage == PAGE_PERF ? "TEMPOS MEDIOS POR PARTE DO QUADRO" : Desc(labels, sSel, dbuf, sizeof dbuf), hint);
}

/* ---- configuracoes de video salvas ao fechar o menu (pen-drive: save/video.cfg; disco: memory card) ---- */
#define VIDEO_CFG "save/video.cfg"
static void VideoSave(void)
{
    uint8_t b[12] = { 'V', 'I', 'D', '4', (uint8_t)sVMode, (uint8_t)sSize, (uint8_t)sAspect, (uint8_t)sAspect480, (uint8_t)sPerf, (uint8_t)sFps, (uint8_t)sFilter, (uint8_t)sColor };
    StorageMkdir("save");
    int fd = StorageOpen(VIDEO_CFG, 1);
    if (fd < 0) return;
    StorageWrite(fd, b, sizeof b);
    StorageClose(fd);
}

static void Apply(void)
{
    BeSetVideoMode(sVMode, sVMode == 1 ? sAspect480 : sAspect, 0); BeSetVideo(sSize, sPerf);
    BeSetScreenFx(sVMode >= 1 ? sFilter : 0);
    BeSetColorFx(sColor);
}

/* monitor de FPS: so o numero, sobre um quadro preto, logo abaixo da vida (energia) no canto da tela. Media dos ultimos 30 quadros. */
static void DrawFps(void)
{
    static uint32_t ts[31]; static int idx, filled;
    ts[idx] = BeCycles(); idx = (idx + 1) % 31; if (filled < 31) filled++;
    int fps = 0;
    if (filled == 31)
    {
        uint32_t dt = ts[(idx + 30) % 31] - ts[idx];            /* 30 intervalos entre o mais antigo e o mais novo */
        if (dt) fps = (int)(30.0f * 294912000.0f / dt + 0.5f);
    }
    char l[8]; snprintf(l, sizeof l, "%d", fps > 99 ? 99 : fps);
    int ux, uy; BeGameToUi(3, 21, &ux, &uy);
    int w = TextWidth(l, 2), h = 14;
    Rect(ux, uy, w + 8, h + 6, 0x000000, 128);
    Text(ux + 4, uy + 3, 2, fps >= 58 ? 0x70FF90 : fps >= 50 ? 0xFFD040 : 0xFF6060, l);
    if (BeWsPad())   /* 16:9 nativo: estado da cena (M modo, S submodo, P pausa, C cutscene, I cena interna, W 1 = laterais liberadas) */
    {
        extern int16_t gMainGameMode, gSubGameMode1; extern uint8_t gPauseScreenFlag, gCurrentCutscene; extern int gRenderWsOn, gRenderIgc;
        char s[40]; snprintf(s, sizeof s, "M%d S%d P%d C%d I%d W%d", (int)gMainGameMode, (int)gSubGameMode1, (int)gPauseScreenFlag, (int)gCurrentCutscene, gRenderIgc, gRenderWsOn);
        Rect(ux, uy + h + 6, TextWidth(s, 1) + 8, 11, 0x000000, 128);
        Text(ux + 4, uy + h + 8, 1, 0xB0B8D0, s);
    }
}

static void DrawOverlays(void)
{
    if (sFps) DrawFps();
    if (sPerfText) DrawPerfOverlay();
}

/* tempos por parte, sobre o jogo (liga em DEBUG GS > PERF TEXT) */
static void DrawPerfOverlay(void)
{
    static const char *nm[6] = { "LOGIC", "SYS", "CACHE", "UPLD", "DRAW", "GS" };
    static const int slot[6] = { 4, 5, 0, 1, 2, 3 };
    char l[40];
    float per = BeProfilePeriodMs();
    int fps = per > 0.1f ? (int)(1000.0f / per + 0.5f) : 0, tp = (int)(per * 10 + 0.5f);
    snprintf(l, sizeof l, "%d FPS %d.%dMS", fps, tp / 10, tp % 10);
    Text(6, 8, 1, 0xFFD040, l);
    for (int i = 0; i < 6; i++)
    {
        int t = (int)(BeProfileMs(slot[i]) * 10 + 0.5f);
        snprintf(l, sizeof l, "%-5s %2d.%d", nm[i], t / 10, t % 10);
        Text(6, 18 + i * 9, 1, i == 5 ? 0x8090B8 : 0xFFFFFF, l);
    }
}

#define AUDIO_CFG "save/audio.cfg"
static void AudioCfgSave(void)
{
    uint8_t b[8] = { 'A', 'U', 'D', '1', (uint8_t)gAudioVol[0], (uint8_t)gAudioVol[1], (uint8_t)gAudioVol[2], (uint8_t)gAudioMono };
    StorageMkdir("save");
    int fd = StorageOpen(AUDIO_CFG, 1);
    if (fd < 0) return;
    StorageWrite(fd, b, sizeof b);
    StorageClose(fd);
}
static void AudioCfgLoad(void)
{
    uint8_t b[8];
    int fd = StorageOpen(AUDIO_CFG, 0);
    if (fd < 0) return;
    int n = StorageRead(fd, b, sizeof b);
    StorageClose(fd);
    if (n < 8 || b[0] != 'A' || b[1] != 'U' || b[2] != 'D' || b[3] != '1' || b[4] > 100 || b[5] > 100 || b[6] > 100) return;
    gAudioVol[0] = b[4]; gAudioVol[1] = b[5]; gAudioVol[2] = b[6]; gAudioMono = b[7] & 1;
}

void MenuVideoLoad(void)
{
    AudioCfgLoad();
    uint8_t b[16];
    int fd = StorageOpen(VIDEO_CFG, 0);
    if (fd < 0) return;
    int n = StorageRead(fd, b, sizeof b);
    StorageClose(fd);
    if (n < 10 || b[0] != 'V' || b[1] != 'I' || b[2] != 'D' || (b[3] != '2' && b[3] != '3' && b[3] != '4')) return;
    if (b[4] > 3 || b[5] > 2 || b[6] > 3 || b[7] > 1) return;
    sVMode = b[4]; sSize = b[5]; sAspect = b[6]; sAspect480 = b[7]; sPerf = b[8] & 1; sFps = b[9] & 1;
    sFilter = (b[3] >= '3' && n >= 11 && b[10] <= 2) ? b[10] : 0;
    sColor = (b[3] == '4' && n >= 12) ? (b[11] & 1) : 0;
    Apply();
    BeSetUiHook((sPerfText || sFps) ? DrawOverlays : 0);
}

void MenuRun(void)
{
#ifdef TEST_MENU
    sPage = TEST_MENU; sSel = 0;
#else
    sPage = PAGE_MAIN; sSel = 0;
#endif
    BeSetUiHook(Draw);
    BeProfileFreeze(1);
    uint16_t prev = PlatformReadPad();
    int waitRelease = 1, closing = 0;
    for (;;)
    {
        PlatformBeat(); RenderFrame();                                   /* mesmo quadro do jogo + menu por cima (e vsync) */
        uint16_t p = PlatformReadPad();
        uint16_t e = p & ~prev; prev = p;
        if (sConfirm)
        {
            if (e & (1 << PB_CROSS)) sConfirm = 0;                       /* manteve */
            else if ((e && !(e & ((1 << PB_UP) | (1 << PB_DOWN)))) || --sConfirmLeft <= 0)   /* desfaz */
            { sVMode = sPrevVMode; sConfirm = 0; Apply(); }
            continue;
        }
        if (closing) { if (!p) break; continue; }        /* so volta ao jogo com todos os botoes soltos */
        if (waitRelease) { if (!(p & ((1 << PB_START) | (1 << PB_SELECT)))) waitRelease = 0; continue; }

        int vids[8], vn = VideoRows(vids);
        int count = sPage == PAGE_PERF ? 1 : sPage == PAGE_MAIN ? 6 : sPage == PAGE_AUDIO ? 5 : sPage == PAGE_DEBUG ? 8 : sPage == PAGE_CONTROL ? GBA_BTN_COUNT + 2 : vn;
        if (e & (1 << PB_UP)) sSel = (sSel + count - 1) % count;
        if (e & (1 << PB_DOWN)) sSel = (sSel + 1) % count;
        int ok = e & (1 << PB_CROSS), back = e & ((1 << PB_CIRCLE) | (1 << PB_START));
        int dir = ((e >> PB_RIGHT) & 1) - ((e >> PB_LEFT) & 1);

        if (sPage == PAGE_MAIN)
        {
            if (back) closing = 1;
            else if (ok && sSel == 0) closing = 1;
            else if (ok && sSel == 1) { sPage = PAGE_VIDEO; sSel = 0; }
            else if (ok && sSel == 2) { sPage = PAGE_AUDIO; sSel = 0; }
            else if (ok && sSel == 3) { sPage = PAGE_CONTROL; sSel = 0; }
            else if (ok && sSel == 4) { sPage = PAGE_DEBUG; sSel = 0; }
            else if (ok && sSel == 5) { sNesStart = 1; closing = 1; }
        }
        else if (sPage == PAGE_AUDIO)
        {
            if (back || (ok && sSel == 4)) { sPage = PAGE_MAIN; sSel = 2; }
            else if (sSel < 3 && dir) { int v = gAudioVol[sSel] + dir * 10; gAudioVol[sSel] = v < 0 ? 0 : v > 100 ? 100 : v; }
            else if (sSel == 3 && (dir || ok)) gAudioMono ^= 1;
        }
        else if (sPage == PAGE_CONTROL)
        {
            if (back || (ok && sSel == GBA_BTN_COUNT + 1)) { PlatformControlsSave(); sPage = PAGE_MAIN; sSel = 3; }
            else if (ok && sSel == GBA_BTN_COUNT) PlatformControlsReset();
            else if (sSel < GBA_BTN_COUNT && (dir || ok))
                gPadMap[sSel] = (uint8_t)((gPadMap[sSel] + (dir ? dir : 1) + PS2_BTN_COUNT) % PS2_BTN_COUNT);
        }
        else if (sPage == PAGE_DEBUG)
        {
            if (back || (ok && sSel == 7)) { sPage = PAGE_MAIN; sSel = 4; }
            else if (ok && sSel == 5) { sPage = PAGE_PERF; sSel = 0; }
            else if (sSel == 6 && (dir || ok)) sPerfText = !sPerfText;
            else if (sSel < 5 && (dir || ok)) gBeDbg[sSel] = !gBeDbg[sSel];
        }
        else if (sPage == PAGE_PERF)
        {
            if (back || ok) { sPage = PAGE_DEBUG; sSel = 5; }
        }
        else
        {
            int id = vids[sSel];
            if (back || (ok && id == V_BACK)) { sPage = PAGE_MAIN; sSel = 1; }
            else if (dir || ok)
            {
                int d = dir ? dir : 1;
                if (id == V_MODE)
                {
                    sPrevVMode = sVMode;
                    sVMode = (sVMode + d + 4) % 4;
                    Apply();
                    if (sVMode >= 2) { sConfirm = 1; sConfirmLeft = 600; }   /* 10 s para confirmar (cabo componente) */
                    sSel = 0;
                }
                else if (id == V_SIZE) { sSize = (sSize + d + 3) % 3; Apply(); }
                else if (id == V_ASPECT) { if (sVMode == 1) sAspect480 ^= 1; else sAspect = (sAspect + d + 4) % 4; Apply(); }
                else if (id == V_COLOR) { sColor ^= 1; Apply(); }
                else if (id == V_FILTER) { sFilter = (sFilter + (dir < 0 ? 2 : 1)) % 3; Apply(); }
                else if (id == V_PERF) { sPerf = !sPerf; Apply(); }
                else if (id == V_FPS) sFps = !sFps;
            }
        }
    }
    BeProfileFreeze(0);
    BeSetUiHook((sPerfText || sFps) ? DrawOverlays : 0);
    VideoSave();
    AudioCfgSave();
    if (sNesStart) { sNesStart = 0; NesRun(); }   /* opcao METROID: emulacao do Metroid de NES (volta pelo menu de pausa dele) */
}

/* ---- tela de aviso (antes do jogo): titulo, linhas de detalhe e rodape. Retorna 1 = tentar de novo, 0 = sair. ---- */
static const char *sMsgTitle, *sMsgLines[4], *sMsgFoot;
static int sMsgN;
static void DrawMessage(void)
{
    Rect(0, 0, 320, 224, 0x000000, 128);
    const int px = 10, py = 50, pw = 300, ph = 124;
    Rect(px - 2, py - 2, pw + 4, ph + 4, 0xFF6060, 128);
    Rect(px, py, pw, ph, 0x301018, 128);
    { const char *h = sMsgN || sMsgFoot[0] ? "AVISO" : "METROID ZERO"; Text(px + (pw - TextWidth(h, 2)) / 2, py + 8, 2, 0xFFD040, h); }
    Rect(px + 10, py + 28, pw - 20, 1, 0xFF6060, 128);
    Text(px + (pw - TextWidth(sMsgTitle, 1)) / 2, py + 38, 1, 0xFFFFFF, sMsgTitle);
    for (int i = 0; i < sMsgN; i++)
        Text(px + (pw - TextWidth(sMsgLines[i], 1)) / 2, py + 54 + i * 12, 1, 0xC0C8E0, sMsgLines[i]);
    Text(px + (pw - TextWidth(sMsgFoot, 1)) / 2, py + ph - 14, 1, 0x70FF90, sMsgFoot);
}

int MenuMessage(const char *title, const char **lines, int n, const char *foot)
{
    sMsgTitle = title; sMsgN = n > 4 ? 4 : n; sMsgFoot = foot;
    for (int i = 0; i < sMsgN; i++) sMsgLines[i] = lines[i];
    BeSetUiHook(DrawMessage);
    uint16_t prev = 0xFFFF; int res = 0;
    for (;;)
    {
        PlatformBeat(); RenderFrame();
        uint16_t p = PlatformReadPad(), e = p & ~prev; prev = p;
        if (e & (1 << PB_CROSS)) { res = 1; break; }
        if (e & (1 << PB_CIRCLE)) { res = 0; break; }
    }
    BeSetUiHook(0);
    return res;
}

/* mostra uma mensagem simples por alguns quadros (ex.: "CARREGANDO") sem esperar tecla */
void MenuStatus(const char *title)
{
    sMsgTitle = title; sMsgN = 0; sMsgFoot = "";
    BeSetUiHook(DrawMessage);
    for (int i = 0; i < 3; i++) { PlatformBeat(); RenderFrame(); }
    BeSetUiHook(0);
}

/* ---- pausa do NES Metroid (START+SELECT dentro do R3): usa o visual do menu por cima do quadro congelado ---- */
int gNesOpt[4] = { 1, 0, 0, 0 };   /* overscan cortado, 4:3, filtro nitido, scanlines off */
static int sNesSel;
static const uint16_t *sNesPix;
static void DrawNesMenu(void)
{
    static const char *const lab[6] = { "CONTINUAR", "REINICIAR NES", "BORDAS (OVERSCAN)", "PROPORCAO", "FILTRO / SCANLINES", "VOLTAR AO ZERO MISSION" };
    Rect(0, 0, 320, 224, 0x000000, 70);
    const int pw = 296, ph = 176, px = 12, py = 24;
    Frame(px, py, pw, ph);
    Tab(px + pw / 2, py + 5, "METROID NES");
    for (int i = 0; i < 6; i++)
    {
        int y = py + 30 + i * 19;
        Bar(px + 8, y, pw - 16, 17, i == sNesSel);
        TextH(px + 26, y + 5, 3, 0xF0F0F0, lab[i]);
        char v[16] = "";
        if (i == 2) snprintf(v, sizeof v, "< %s >", gNesOpt[0] ? "CORTAR" : "COMPLETO");
        if (i == 3) snprintf(v, sizeof v, "< %s >", gNesOpt[1] ? "16:9" : "4:3");
        if (i == 4) snprintf(v, sizeof v, "< %s >", gNesOpt[3] ? "SCAN" : gNesOpt[2] ? "SUAVE" : "NITIDO");
        if (v[0]) DrawValue(px + pw - 20, y + 5, v, C_LIME, 0xFFE070);
    }
    DescBar(px, py + ph - 22, pw, "VOLTA AO JOGO DO ZERO MISSION", "X: ESCOLHER   O: CONTINUAR");
}
int MenuNes(const uint16_t *pix)
{
    sNesSel = 0; sNesPix = pix;
    uint16_t prev = 0xFFFF; int res = 0;
    for (;;)
    {
        PlatformBeat();
        BeNesPresent(pix, gNesOpt[0], gNesOpt[1], gNesOpt[2], gNesOpt[3], DrawNesMenu);
        uint16_t p = PlatformReadPad(), e = p & ~prev; prev = p;
        if (e & (1 << PB_UP)) sNesSel = (sNesSel + 5) % 6;
        if (e & (1 << PB_DOWN)) sNesSel = (sNesSel + 1) % 6;
        int d = (e & (1 << PB_RIGHT)) ? 1 : (e & (1 << PB_LEFT)) ? -1 : 0, ok = (e & (1 << PB_CROSS)) != 0;
        if (e & (1 << PB_CIRCLE)) break;
        if (sNesSel == 2 && (d || ok)) gNesOpt[0] ^= 1;
        if (sNesSel == 3 && (d || ok)) gNesOpt[1] ^= 1;
        if (sNesSel == 4 && (d || ok))   /* NITIDO -> SUAVE -> SCAN */
        {
            int s = gNesOpt[3] ? 2 : gNesOpt[2] ? 1 : 0; s = (s + (d < 0 ? 2 : 1)) % 3;
            gNesOpt[2] = s == 1; gNesOpt[3] = s == 2;
        }
        if (ok && sNesSel == 0) break;
        if (ok && sNesSel == 1) { res = 1; break; }
        if (ok && sNesSel == 5) { res = 2; break; }
    }
    return res;
}
