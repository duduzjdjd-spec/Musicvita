// MusicVita - musica de Internet Archive para PS Vita (homebrew, no oficial)
//
//  - Artistas: Live Music Archive (coleccion "etree"): conciertos que las bandas permiten compartir.
//  - Generos:  coleccion "netlabels": musica de sellos independientes con licencia Creative Commons.
//  - Buscar:   en las dos colecciones, por artista o por album.
//  - Portadas: archive.org/services/img/<identificador>
//  - Reproductor: MP3 por streaming (HTTP Range), decodificado con minimp3 (CC0, github.com/lieff/minimp3).
//
// Controles del reproductor: X/CUADRADO pausa, izquierda/derecha 10 s, arriba/abajo 1 min,
// L/R pista anterior/siguiente, O volver.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <malloc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/gxm.h>
#include <psp2/ime_dialog.h>
#include <psp2/apputil.h>
#include <curl/curl.h>
#include <vita2d.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// ---- Internet Archive (si cambia algo, editalo aqui) ----
#define IA_SEARCH "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title" \
                  "&fl%%5B%%5D=creator&rows=%d&page=1&sort%%5B%%5D=%s&output=json"
#define IA_META   "https://archive.org/metadata/%s"
#define IA_IMG    "https://archive.org/services/img/%s"
#define IA_DL     "https://archive.org/download/%s/%s"

#define Q_ARTISTS "collection:etree AND mediatype:collection"
#define Q_ARTIST  "collection:%s"
#define Q_GENRE   "collection:netlabels AND mediatype:audio AND subject:\"%s\""
#define Q_SEARCH  "(collection:etree OR collection:netlabels) AND (mediatype:audio OR mediatype:etree) AND (title:(%s) OR creator:(%s))"

int _newlib_heap_size_user = 48 * 1024 * 1024;

static vita2d_pgf *pgf;
static SceCtrlData pad, oldpad;

static uint64_t now_us(void) { return (uint64_t)sceKernelGetProcessTimeWide(); }

static void status(const char *msg) {
  vita2d_start_drawing(); vita2d_clear_screen();
  vita2d_pgf_draw_text(pgf, 40, 270, RGBA8(255,255,255,255), 1.2f, msg);
  vita2d_end_drawing(); vita2d_swap_buffers();
}

// ---------- HTTP (libcurl + OpenSSL) ----------
static int g_step = 0, g_err = 0, g_status = 0;
static volatile int g_abort = 0;
struct mem { char *b; size_t n, cap; };

static size_t wcb(void *p, size_t sz, size_t nm, void *u) {
  struct mem *m = u; size_t t = sz * nm;
  if (m->n + t + 1 > m->cap) {
    size_t nc = m->cap ? m->cap : 256 * 1024;
    while (nc < m->n + t + 1) nc *= 2;
    if (nc > 16 * 1024 * 1024) return 0;
    char *nb = realloc(m->b, nc);
    if (!nb) return 0;
    m->b = nb; m->cap = nc;
  }
  memcpy(m->b + m->n, p, t); m->n += t; m->b[m->n] = 0;
  return t;
}

static int xferinfo(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  return g_abort ? 1 : 0;
}

static char *http_req_ex(const char *url, int *outlen, int bearer, int quiet) {
  if (!quiet) { g_step = 1; g_err = 0; g_status = 0; }
  CURL *c = curl_easy_init();
  if (!c) { g_err = -1; return NULL; }
  struct curl_slist *hdr = NULL;
  struct mem m = {0, 0, 0};
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 PlutoVita");
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo);
  if (!quiet) g_step = 4;
  CURLcode rc = curl_easy_perform(c);
  long st = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
  if (!quiet) { g_err = (int)rc; g_status = (int)st; }
  curl_easy_cleanup(c);
  if (hdr) curl_slist_free_all(hdr);
  if (rc != CURLE_OK || st >= 400) { free(m.b); return NULL; }
  if (outlen) *outlen = (int)m.n;
  return m.b;
}
static char *http_get(const char *url, int *outlen) { return http_req_ex(url, outlen, 0, 0); }

static int put_utf8(char *d, int room, unsigned cp) {
  if (cp < 0x80)    { if (room < 1) return 0; d[0] = cp; return 1; }
  if (cp < 0x800)   { if (room < 2) return 0; d[0] = 0xC0 | (cp >> 6); d[1] = 0x80 | (cp & 0x3F); return 2; }
  if (cp < 0x10000) { if (room < 3) return 0; d[0] = 0xE0 | (cp >> 12); d[1] = 0x80 | ((cp >> 6) & 0x3F); d[2] = 0x80 | (cp & 0x3F); return 3; }
  if (room < 4) return 0;
  d[0] = 0xF0 | (cp >> 18); d[1] = 0x80 | ((cp >> 12) & 0x3F); d[2] = 0x80 | ((cp >> 6) & 0x3F); d[3] = 0x80 | (cp & 0x3F);
  return 4;
}

static void copy_json(const char *p, char *dst, int max) {
  int i = 0;
  while (*p && *p != '"' && i < max - 1) {
    if (*p == '\\' && p[1]) {
      if (p[1] == 'u' && p[2] && p[3] && p[4] && p[5]) {
        char hx[5] = { p[2], p[3], p[4], p[5], 0 };
        unsigned cp = (unsigned)strtol(hx, NULL, 16);
        p += 6;
        if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {     // par sustituto
          char h2[5] = { p[2], p[3], p[4], p[5], 0 };
          unsigned lo = (unsigned)strtol(h2, NULL, 16);
          if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
        }
        int w = put_utf8(dst + i, max - 1 - i, cp);
        if (!w) break;
        i += w; continue;
      }
      char e = p[1];
      dst[i++] = e == 'n' ? ' ' : e == 't' ? ' ' : e;
      p += 2; continue;
    }
    dst[i++] = *p++;
  }
  dst[i] = 0;
}
// ---------- Buscador ----------
// minusculas y sin acentos, para comparar ("Accion" encuentra "Acción")
static void fold(const char *s, char *o, int cap) {
  int i = 0;
  const unsigned char *u = (const unsigned char *)s;
  while (*u && i < cap - 1) {
    unsigned c = *u;
    if (c == 0xC3 && u[1]) {
      unsigned d = u[1] | 0x20;                 // 0x80-0x9F mayusculas -> 0xA0-0xBF
      char r = 0;
      if (d >= 0xA0 && d <= 0xA5) r = 'a'; else if (d == 0xA7) r = 'c';
      else if (d >= 0xA8 && d <= 0xAB) r = 'e'; else if (d >= 0xAC && d <= 0xAF) r = 'i';
      else if (d == 0xB1) r = 'n'; else if (d >= 0xB2 && d <= 0xB6) r = 'o';
      else if (d >= 0xB9 && d <= 0xBC) r = 'u';
      if (r) { o[i++] = r; u += 2; continue; }
    }
    o[i++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    u++;
  }
  o[i] = 0;
}

static void utf8_to_utf16(const char *s, SceWChar16 *o, int cap) {
  int i = 0;
  const unsigned char *u = (const unsigned char *)s;
  while (*u && i < cap - 1) {
    unsigned cp;
    if (*u < 0x80) cp = *u++;
    else if ((*u & 0xE0) == 0xC0 && u[1]) { cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); u += 2; }
    else if ((*u & 0xF0) == 0xE0 && u[1] && u[2]) { cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); u += 3; }
    else { u++; continue; }
    o[i++] = (SceWChar16)cp;
  }
  o[i] = 0;
}

static void utf16_to_utf8(const SceWChar16 *s, char *o, int cap) {
  int i = 0;
  for (; *s; s++) { int w = put_utf8(o + i, cap - 1 - i, *s); if (!w) break; i += w; }
  o[i] = 0;
}

// Abre el teclado de la Vita; devuelve 1 si el usuario confirmo
static int ime_search_t(const char *title_txt, char *query, int cap) {
  static SceWChar16 title[32], init[128], buf[128];
  utf8_to_utf16(title_txt, title, 32);
  utf8_to_utf16(query, init, 128);
  memset(buf, 0, sizeof buf);
  SceImeDialogParam ip;
  sceImeDialogParamInit(&ip);
  ip.supportedLanguages = SCE_IME_LANGUAGE_SPANISH | SCE_IME_LANGUAGE_ENGLISH;
  ip.languagesForced = SCE_FALSE;
  ip.type = SCE_IME_TYPE_DEFAULT;
  ip.option = 0;
  ip.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  ip.title = title;
  ip.maxTextLength = 60;
  ip.initialText = init;
  ip.inputTextBuffer = buf;
  if (sceImeDialogInit(&ip) < 0) return 0;
  int ok = 0;
  for (;;) {
    vita2d_start_drawing(); vita2d_clear_screen(); vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
    if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      SceImeDialogResult r; memset(&r, 0, sizeof r);
      sceImeDialogGetResult(&r);
      ok = r.button == SCE_IME_DIALOG_BUTTON_ENTER;
      sceImeDialogTerm();
      break;
    }
  }
  if (ok) utf16_to_utf8(buf, query, cap);
  sceCtrlPeekBufferPositive(0, &pad, 1); oldpad = pad;   // que la X del teclado no abra nada
  return ok;
}

static int ime_search(char *query, int cap) { return ime_search_t("Buscar", query, cap); }

// ---------- Imagenes (logos y portadas) ----------
// Un hilo baja las imagenes que pide la pantalla (la seleccionada primero) y las guarda como texturas.
enum { IM_REQ = 1, IM_LOADING, IM_READY, IM_FAIL };
typedef struct { char url[240]; vita2d_texture *tex; int state; uint32_t want, used; } Img;
#define IMG_MAX 96
static Img img_tab[IMG_MAX];
static SceUID img_mtx = -1;
static uint32_t img_tick, img_frame;
static volatile int img_run;

static void img_lock(void)   { sceKernelLockMutex(img_mtx, 1, NULL); }
static void img_unlock(void) { sceKernelUnlockMutex(img_mtx, 1); }

// Pide (si hace falta) y devuelve la textura si ya esta lista. Solo desde el hilo principal.
static vita2d_texture *img_get(const char *url) {
  if (!url || !url[0] || img_mtx < 0) return NULL;
  vita2d_texture *t = NULL;
  int freei = -1;
  img_lock();
  for (int i = 0; i < IMG_MAX; i++) {
    Img *m = &img_tab[i];
    if (!m->state) { if (freei < 0) freei = i; continue; }
    if (!strcmp(m->url, url)) {
      m->used = img_frame;
      if (m->state == IM_READY) t = m->tex;
      else if (m->state == IM_REQ) m->want = ++img_tick;
      img_unlock();
      return t;
    }
  }
  if (freei >= 0) {
    Img *m = &img_tab[freei];
    snprintf(m->url, sizeof m->url, "%s", url);
    m->tex = NULL; m->state = IM_REQ; m->want = ++img_tick; m->used = img_frame;
  }
  img_unlock();
  return NULL;
}

// Libera las que no se ven hace rato. Llamar con la GPU detenida (despues de vita2d_wait_rendering_done).
static void img_gc(void) {
  if (img_mtx < 0) return;
  img_frame++;
  img_lock();
  int used = 0;
  for (int i = 0; i < IMG_MAX; i++) if (img_tab[i].state) used++;
  while (used > IMG_MAX - 12) {
    int v = -1;
    for (int i = 0; i < IMG_MAX; i++) {
      Img *m = &img_tab[i];
      if (!m->state || m->state == IM_LOADING || img_frame - m->used < 30) continue;
      if (v < 0 || (int32_t)(m->used - img_tab[v].used) < 0) v = i;
    }
    if (v < 0) break;
    if (img_tab[v].tex) vita2d_free_texture(img_tab[v].tex);
    memset(&img_tab[v], 0, sizeof img_tab[v]);
    used--;
  }
  img_unlock();
}

static int img_thread(SceSize a, void *u) {
  static char url[300];
  while (img_run) {
    int pick = -1;
    img_lock();
    for (int i = 0; i < IMG_MAX; i++) {
      Img *m = &img_tab[i];
      if (m->state != IM_REQ || img_frame - m->used > 2) continue;  // solo lo que se esta viendo
      if (pick < 0 || m->want > img_tab[pick].want) pick = i;
    }
    if (pick >= 0) {
      img_tab[pick].state = IM_LOADING;
      snprintf(url, sizeof url, "%s", img_tab[pick].url);
    }
    img_unlock();
    if (pick < 0) { sceKernelDelayThread(30000); continue; }
    vita2d_texture *t = NULL;
    for (int attempt = 0; attempt < 2 && !t && img_run; attempt++) {   // 2 intentos
      int n = 0;
      uint8_t *d = (uint8_t *)http_req_ex(url, &n, 0, 1);
      if (d && n > 8) {
        if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') t = vita2d_load_PNG_buffer(d);
        else if (d[0] == 0xFF && d[1] == 0xD8) t = vita2d_load_JPEG_buffer(d, n);
      }
      free(d);
    }
    if (t) vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    img_lock();
    img_tab[pick].tex = t;
    img_tab[pick].state = t ? IM_READY : IM_FAIL;
    img_unlock();
  }
  return 0;
}

// Dibuja la textura dentro de la caja (x, y, w, h) sin deformarla
static void draw_fit(vita2d_texture *t, float x, float y, float w, float h) {
  float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
  if (tw < 1 || th < 1) return;
  float sc = w / tw < h / th ? w / tw : h / th;
  vita2d_draw_texture_scale(t, x + (w - tw * sc) / 2, y + (h - th * sc) / 2, sc, sc);
}

// Recorta el texto para que quepa en "maxw" pixeles
static const char *fit_text(const char *s, float scale, float maxw, char *buf, int cap) {
  if (vita2d_pgf_text_width(pgf, scale, s) <= maxw) return s;
  snprintf(buf, cap, "%s", s);
  int n = strlen(buf);
  while (n > 0) {
    n--; while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;   // no partir letras con acento
    buf[n] = 0;
    char t[200]; snprintf(t, sizeof t, "%s...", buf);
    if (vita2d_pgf_text_width(pgf, scale, t) <= maxw) { snprintf(buf, cap, "%s", t); break; }
  }
  return buf;
}

// ---------- Pantalla de lista generica (con buscador) ----------
// Devuelve el indice elegido, o LS_BACK (O), LS_MODE (SELECT), LS_EXIT (START)
enum { LS_BACK = -1, LS_MODE = -2, LS_EXIT = -3, LS_SEARCH = -4 };
typedef struct { int sel, top; char query[128]; } ListState;

// global_search: TRIANGULO no filtra esta lista, devuelve LS_SEARCH para que el llamador busque en todo
// imgs: URL de imagen por elemento (o NULL). logos: 1 = logos de canal (tambien en cada fila), 0 = portadas
static int list_screen_img(const char *title, const char *const *names, const char *const *keys, const char *const *imgs, int logos,
                           int n, ListState *ls, const char *hint, int global_search) {
  int *view = malloc(sizeof(int) * (n > 0 ? n : 1));
  if (!view) return LS_BACK;
  int nview = 0, rebuild = 1, ret = LS_BACK;
  const int rows = 14;
  for (;;) {
    if (rebuild) {
      char q[128], nm[160];
      fold(ls->query, q, sizeof q);
      nview = 0;
      for (int i = 0; i < n; i++) {
        if (q[0]) { fold(keys ? keys[i] : names[i], nm, sizeof nm); if (!strstr(nm, q)) continue; }
        view[nview++] = i;
      }
      if (ls->sel >= nview) ls->sel = nview > 0 ? nview - 1 : 0;
      rebuild = 0;
    }
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned p = pad.buttons & ~oldpad.buttons; oldpad = pad;
    if ((p & SCE_CTRL_DOWN) && ls->sel < nview - 1) ls->sel++;
    if ((p & SCE_CTRL_UP) && ls->sel > 0) ls->sel--;
    if (p & SCE_CTRL_RTRIGGER) ls->sel = ls->sel + rows < nview ? ls->sel + rows : nview - 1;
    if (p & SCE_CTRL_LTRIGGER) ls->sel = ls->sel - rows > 0 ? ls->sel - rows : 0;
    if (ls->sel < 0) ls->sel = 0;
    if (ls->sel < ls->top) ls->top = ls->sel;
    if (ls->sel >= ls->top + rows) ls->top = ls->sel - rows + 1;
    if ((p & SCE_CTRL_CROSS) && nview > 0) { ret = view[ls->sel]; break; }
    if (p & SCE_CTRL_CIRCLE) { ret = LS_BACK; break; }
    if (p & SCE_CTRL_SELECT) { ret = LS_MODE; break; }
    if (p & SCE_CTRL_START) { ret = LS_EXIT; break; }
    if ((p & SCE_CTRL_TRIANGLE) && global_search) { ret = LS_SEARCH; break; }
    if (p & SCE_CTRL_TRIANGLE) { if (ime_search(ls->query, sizeof ls->query)) { ls->sel = ls->top = 0; rebuild = 1; } continue; }
    if ((p & SCE_CTRL_SQUARE) && ls->query[0]) { ls->query[0] = 0; ls->sel = ls->top = 0; rebuild = 1; }

    vita2d_wait_rendering_done();       // la GPU termino el cuadro anterior: ya se pueden liberar imagenes
    img_gc();
    vita2d_start_drawing(); vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, 960, 50, RGBA8(30,30,120,255));
    vita2d_pgf_draw_text(pgf, 20, 34, RGBA8(255,255,255,255), 1.2f, title);
    char hdr[200];
    if (ls->query[0]) snprintf(hdr, sizeof hdr, "Buscar: %s  (%d)", ls->query, nview);
    else snprintf(hdr, sizeof hdr, "%d", n);
    vita2d_pgf_draw_text(pgf, 960 - 20 - vita2d_pgf_text_width(pgf, 1.0f, hdr), 34, RGBA8(255,230,120,255), 1.0f, hdr);
    float listw = imgs ? 600 : 940, tx = (imgs && logos) ? 78 : 24;
    char cut[200];
    for (int i = 0; i < rows && ls->top + i < nview; i++) {
      int y = 66 + i * 31, idx = ls->top + i;
      if (idx == ls->sel) vita2d_draw_rectangle(10, y - 4, listw, 30, RGBA8(70,70,200,255));
      if (imgs && logos) {                                   // logo chico en cada fila
        vita2d_texture *t = img_get(imgs[view[idx]]);
        if (t) draw_fit(t, 16, y - 3, 54, 28);
      }
      vita2d_pgf_draw_text(pgf, tx, y + 18, RGBA8(255,255,255,255), 1.0f,
                           fit_text(names[view[idx]], 1.0f, listw - tx, cut, sizeof cut));
    }
    if (imgs && nview > 0) {                                 // imagen grande del seleccionado
      vita2d_draw_rectangle(625, 62, 320, 432, RGBA8(25,25,45,255));
      vita2d_texture *t = img_get(imgs[view[ls->sel]]);
      if (t) draw_fit(t, 635, 72, 300, logos ? 220 : 412);
      else vita2d_pgf_draw_text(pgf, 700, 270, RGBA8(120,120,150,255), 1.0f,
                                imgs[view[ls->sel]] && imgs[view[ls->sel]][0] ? "Cargando imagen..." : "Sin imagen");
      if (logos) vita2d_pgf_draw_text(pgf, 640, 330, RGBA8(255,255,255,255), 1.0f,
                                      fit_text(names[view[ls->sel]], 1.0f, 295, cut, sizeof cut));
    }
    if (nview == 0) vita2d_pgf_draw_text(pgf, 24, 100, RGBA8(255,200,200,255), 1.0f,
                                         ls->query[0] ? "Sin resultados. CUADRADO: ver todos" : "Lista vacia");
    vita2d_pgf_draw_text(pgf, 20, 512, RGBA8(180,180,180,255), 0.85f, hint);
    vita2d_pgf_draw_text(pgf, 20, 536, RGBA8(180,180,180,255), 0.85f,
                         ls->query[0] ? "TRIANGULO: buscar   CUADRADO: ver todos   L/R: pagina   START: salir"
                                      : "TRIANGULO: buscar   L/R: pagina   START: salir");
    vita2d_end_drawing(); vita2d_swap_buffers();
  }
  free(view);
  return ret;
}
static int list_screen_ex(const char *title, const char *const *names, const char *const *keys, int n, ListState *ls, const char *hint, int global_search) {
  return list_screen_img(title, names, keys, NULL, 0, n, ls, hint, global_search);
}
static int list_screen(const char *title, const char *const *names, int n, ListState *ls, const char *hint) {
  return list_screen_ex(title, names, NULL, n, ls, hint, 0);
}

static void show_error(const char *what) {
  char m[220];
  snprintf(m, sizeof m, "%s: http %d  curl %d  (O = volver)", what, g_status, g_err);
  for (;;) {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned p = pad.buttons & ~oldpad.buttons; oldpad = pad;
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) break;
    status(m);
  }
}

// ---------- JSON minimo (recorre el texto sin copiarlo) ----------
static const char *jws(const char *p) { while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; return p; }

static const char *jskip(const char *p) {          // salta un valor completo
  p = jws(p);
  if (!p || !*p) return NULL;
  if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } return *p ? p + 1 : NULL; }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (*p) {
      if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; } if (!*p) return NULL; }
      else if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
      p++;
    }
    return NULL;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
  return p;
}

// valor de "key" dentro del objeto que empieza en p (solo hijos directos)
static const char *jget(const char *p, const char *key) {
  p = jws(p);
  if (!p || *p != '{') return NULL;
  int kl = strlen(key);
  p++;
  for (;;) {
    p = jws(p);
    if (!p || *p != '"') return NULL;
    const char *k = p + 1;
    const char *ke = jskip(p);
    if (!ke) return NULL;
    int match = (ke - 1 - k) == kl && !strncmp(k, key, kl);
    p = jws(ke);
    if (!p || *p != ':') return NULL;
    p = jws(p + 1);
    if (match) return p;
    p = jws(jskip(p));
    if (!p || *p != ',') return NULL;
    p++;
  }
}

// busca "key" a cualquier profundidad (primer resultado cuyo valor sea del tipo pedido: '[' '{' o 0 = cualquiera)
static const char *jfind(const char *p, const char *key, char want, int depth) {
  p = jws(p);
  if (!p || depth > 8) return NULL;
  if (*p == '{') {
    const char *v = jget(p, key);
    if (v && (!want || *v == want)) return v;
    p++;
    for (;;) {
      p = jws(p);
      if (!p || *p != '"') return NULL;
      p = jws(jskip(p)); if (!p || *p != ':') return NULL;
      p = jws(p + 1);
      const char *r = jfind(p, key, want, depth + 1);
      if (r) return r;
      p = jws(jskip(p));
      if (!p || *p != ',') return NULL;
      p++;
    }
  }
  if (*p == '[') {
    p = jws(p + 1);
    while (p && *p && *p != ']') {
      const char *r = jfind(p, key, want, depth + 1);
      if (r) return r;
      p = jws(jskip(p));
      if (!p || *p != ',') break;
      p = jws(p + 1);
    }
  }
  return NULL;
}

static const char *jarr_first(const char *p) { p = jws(p); if (!p || *p != '[') return NULL; p = jws(p + 1); return (p && *p && *p != ']') ? p : NULL; }
static const char *jarr_next(const char *p) { p = jws(jskip(p)); if (!p || *p != ',') return NULL; p = jws(p + 1); return (p && *p && *p != ']') ? p : NULL; }

// copia el texto (o numero) de un valor
static void jstr(const char *v, char *dst, int cap) {
  dst[0] = 0;
  v = jws(v);
  if (!v) return;
  if (*v == '"') { copy_json(v + 1, dst, cap); return; }
  int i = 0;
  while (*v && *v != ',' && *v != '}' && *v != ']' && i < cap - 1) dst[i++] = *v++;
  dst[i] = 0;
}

// texto de un valor; si es un arreglo (["a","b"]) toma el primero
static void jtext(const char *v, char *dst, int cap) {
  dst[0] = 0;
  v = jws(v);
  if (!v) return;
  if (*v == '[') { const char *f = jarr_first(v); if (f) jstr(f, dst, cap); return; }
  jstr(v, dst, cap);
}

// ---------- Listas ----------
typedef struct { char id[96]; char name[200]; char key[200]; char img[160]; } Entry;
typedef struct { Entry *e; const char **names, **keys, **imgs; int n, cap; } EntryList;

static void el_free(EntryList *l) { free(l->e); free(l->names); free(l->keys); free(l->imgs); memset(l, 0, sizeof *l); }
static Entry *el_add(EntryList *l) {
  if (l->n == l->cap) {
    int nc = l->cap ? l->cap * 2 : 64;
    Entry *ne = realloc(l->e, sizeof(Entry) * nc);
    if (!ne) return NULL;
    l->e = ne; l->cap = nc;
  }
  memset(&l->e[l->n], 0, sizeof(Entry));
  return &l->e[l->n++];
}
static void el_names(EntryList *l) {
  free(l->names); free(l->keys); free(l->imgs);
  int n = l->n > 0 ? l->n : 1;
  l->names = malloc(sizeof(char *) * n); l->keys = malloc(sizeof(char *) * n); l->imgs = malloc(sizeof(char *) * n);
  for (int i = 0; i < l->n; i++) {
    if (l->names) l->names[i] = l->e[i].name;
    if (l->keys)  l->keys[i]  = l->e[i].key[0] ? l->e[i].key : l->e[i].name;
    if (l->imgs)  l->imgs[i]  = l->e[i].img;
  }
}

// ---------- Internet Archive ----------
static void url_enc(const char *s, char *o, int cap, int keep_slash) {
  static const char hx[] = "0123456789ABCDEF";
  int i = 0;
  for (; *s && i < cap - 4; s++) {
    unsigned char c = (unsigned char)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) o[i++] = c;
    else { o[i++] = '%'; o[i++] = hx[c >> 4]; o[i++] = hx[c & 15]; }
  }
  o[i] = 0;
}

// Busqueda avanzada -> lista (identificador, titulo, artista, portada)
static int ia_search(const char *q, const char *sort, int rows, int with_creator, EntryList *out) {
  static char qe[2048], url[2600];
  char se[64];
  url_enc(q, qe, sizeof qe, 0);
  url_enc(sort, se, sizeof se, 0);
  snprintf(url, sizeof url, IA_SEARCH, qe, rows, se);
  char *doc = http_get(url, NULL);
  if (!doc) return -1;
  const char *docs = jfind(doc, "docs", '[', 0);
  for (const char *d = jarr_first(docs); d; d = jarr_next(d)) {
    if (*jws(d) != '{') continue;
    char id[96], title[160], creator[96];
    const char *v;
    id[0] = title[0] = creator[0] = 0;
    if ((v = jget(d, "identifier"))) jtext(v, id, sizeof id);
    if ((v = jget(d, "title"))) jtext(v, title, sizeof title);
    if ((v = jget(d, "creator"))) jtext(v, creator, sizeof creator);
    if (!id[0]) continue;
    if (!title[0]) snprintf(title, sizeof title, "%s", id);
    Entry *e = el_add(out); if (!e) break;
    snprintf(e->id, sizeof e->id, "%s", id);
    if (with_creator && creator[0] && !strstr(title, creator)) snprintf(e->name, sizeof e->name, "%s - %s", creator, title);
    else snprintf(e->name, sizeof e->name, "%s", title);
    snprintf(e->img, sizeof e->img, IA_IMG, id);
  }
  free(doc);
  el_names(out);
  return out->n ? 0 : -1;
}

// ---- Pistas de un album ----
typedef struct { char file[256]; char title[128]; float len; long long size; int track; } Track;
static Track *tracks; static int ntracks;
static char alb_id[96], alb_title[160], alb_creator[96];

static float parse_len(const char *s) {          // "303.15" o "5:03" o "1:02:03"
  if (!s[0]) return 0;
  if (!strchr(s, ':')) return (float)atof(s);
  float t = 0; const char *p = s;
  while (*p) { t = t * 60 + (float)atof(p); p = strchr(p, ':'); if (!p) break; p++; }
  return t;
}

static int track_cmp(const void *a, const void *b) {
  const Track *x = a, *y = b;
  if (x->track && y->track && x->track != y->track) return x->track - y->track;
  return strcmp(x->file, y->file);
}

static int ends_mp3(const char *n) { int l = strlen(n); return l > 4 && !strcasecmp(n + l - 4, ".mp3"); }

static int load_tracks(const char *id) {
  free(tracks); tracks = NULL; ntracks = 0;
  snprintf(alb_id, sizeof alb_id, "%s", id);
  alb_title[0] = alb_creator[0] = 0;
  char url[300]; snprintf(url, sizeof url, IA_META, id);
  char *doc = http_get(url, NULL);
  if (!doc) return -1;
  const char *meta = jget(doc, "metadata"), *v;
  if (meta) {
    if ((v = jget(meta, "title"))) jtext(v, alb_title, sizeof alb_title);
    if ((v = jget(meta, "creator"))) jtext(v, alb_creator, sizeof alb_creator);
  }
  const char *files = jget(doc, "files");
  // elige un solo formato MP3 para no repetir pistas
  static const char *prefs[] = { "VBR MP3", "MP3", "128Kbps MP3", "256Kbps MP3", "320Kbps MP3", "192Kbps MP3", "64Kbps MP3" };
  int best = -1;
  for (const char *f = jarr_first(files); f; f = jarr_next(f)) {
    char fmt[48], name[256];
    if ((v = jget(f, "format"))) jtext(v, fmt, sizeof fmt); else continue;
    if ((v = jget(f, "name"))) jtext(v, name, sizeof name); else continue;
    if (!ends_mp3(name)) continue;
    for (int k = 0; k < 7; k++) if (!strcmp(fmt, prefs[k]) && (best < 0 || k < best)) best = k;
  }
  int cap = 0;
  for (const char *f = jarr_first(files); f && best >= 0; f = jarr_next(f)) {
    char fmt[48], buf[64];
    if ((v = jget(f, "format"))) jtext(v, fmt, sizeof fmt); else continue;
    if (strcmp(fmt, prefs[best])) continue;
    if (ntracks == cap) {
      cap = cap ? cap * 2 : 32;
      Track *nt = realloc(tracks, sizeof(Track) * cap);
      if (!nt) break;
      tracks = nt;
    }
    Track *t = &tracks[ntracks];
    memset(t, 0, sizeof *t);
    if ((v = jget(f, "name"))) jtext(v, t->file, sizeof t->file);
    if (!ends_mp3(t->file)) continue;
    if ((v = jget(f, "title"))) jtext(v, t->title, sizeof t->title);
    if (!t->title[0]) {                                  // sin titulo: el nombre del archivo
      const char *b = strrchr(t->file, '/'); b = b ? b + 1 : t->file;
      snprintf(t->title, sizeof t->title, "%s", b);
      char *dot = strrchr(t->title, '.'); if (dot) *dot = 0;
    }
    if ((v = jget(f, "length"))) { jtext(v, buf, sizeof buf); t->len = parse_len(buf); }
    if ((v = jget(f, "size"))) { jtext(v, buf, sizeof buf); t->size = atoll(buf); }
    if ((v = jget(f, "track"))) { jtext(v, buf, sizeof buf); t->track = atoi(buf); }
    ntracks++;
  }
  free(doc);
  if (ntracks > 1) qsort(tracks, ntracks, sizeof(Track), track_cmp);
  return ntracks ? 0 : -1;
}

// =====================================================================
//                    REPRODUCTOR (streaming MP3)
// =====================================================================
// Hilo de red: baja el archivo (desde un byte dado) a un anillo de 1 MB; si se corta, sigue donde quedo.
// Hilo de audio: saca bytes del anillo, decodifica con minimp3 y los manda a sceAudioOut.
// s_gen cambia con cada pista, salto o parada: todo lo de antes se descarta.
#define RING_SZ (1024 * 1024)
static uint8_t *ring;
static int r_rd, r_wr, r_cnt;
static SceUID p_mtx = -1;
static void plock(void)   { sceKernelLockMutex(p_mtx, 1, NULL); }
static void punlock(void) { sceKernelUnlockMutex(p_mtx, 1); }

static volatile int app_run = 1;
static volatile int s_gen, s_eof_gen = -1, s_err;
static char s_url[1200];
static long long s_off;
static volatile int p_active, p_paused, p_end_gen = -1;
static volatile double p_base;
static volatile long long p_samples;
static volatile int p_rate, p_kbps;

typedef struct { int gen, checked; long long pos, want; CURL *c; } SCtx;

static size_t s_write(void *ptr, size_t sz, size_t nm, void *u) {
  SCtx *x = u;
  size_t t = sz * nm, done = 0;
  const uint8_t *d = ptr;
  if (!x->checked) {                         // si el servidor ignora el rango (200), salta hasta "want"
    long code = 0; curl_easy_getinfo(x->c, CURLINFO_RESPONSE_CODE, &code);
    x->checked = 1;
    if (code >= 400) return 0;
    if (code == 200) x->pos = 0;
  }
  while (done < t && x->pos < x->want) {
    long long k = x->want - x->pos; if (k > (long long)(t - done)) k = t - done;
    done += k; x->pos += k;
  }
  while (done < t) {
    if (x->gen != s_gen || !app_run) return 0;
    plock();
    int n = 0;
    if (x->gen == s_gen) {
      int room = RING_SZ - r_cnt;
      n = (int)(t - done) < room ? (int)(t - done) : room;
      int first = RING_SZ - r_wr < n ? RING_SZ - r_wr : n;
      memcpy(ring + r_wr, d + done, first);
      if (n > first) memcpy(ring, d + done + first, n - first);
      r_wr = (r_wr + n) % RING_SZ; r_cnt += n;
    }
    punlock();
    done += n; x->pos += n;
    if (n == 0) sceKernelDelayThread(10000);   // anillo lleno: espera a que se consuma
  }
  return t;
}

static int s_xfer(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  return ((int)(intptr_t)u != s_gen || !app_run) ? 1 : 0;
}

static int stream_thread(SceSize a, void *u) {
  static char url[1200];
  int handled = -1;
  while (app_run) {
    int gen = s_gen;
    if (gen == handled || !s_url[0]) { sceKernelDelayThread(10000); continue; }
    handled = gen;
    plock(); snprintf(url, sizeof url, "%s", s_url); long long off = s_off; punlock();
    int tries = 0;
    for (;;) {
      CURL *c = curl_easy_init();
      if (!c) { s_err = -1; s_eof_gen = gen; break; }
      SCtx x; memset(&x, 0, sizeof x);
      x.gen = gen; x.pos = off; x.want = off; x.c = c;
      char range[48];
      curl_easy_setopt(c, CURLOPT_URL, url);
      curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, s_write);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
      curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
      curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(c, CURLOPT_USERAGENT, "MusicVita/1.0 (PS Vita homebrew)");
      curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
      curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);     // sin tiempo total: una cancion larga tarda
      curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
      curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, s_xfer);
      curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)(intptr_t)gen);
      if (off > 0) { snprintf(range, sizeof range, "%lld-", off); curl_easy_setopt(c, CURLOPT_RANGE, range); }
      CURLcode rc = curl_easy_perform(c);
      long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
      curl_easy_cleanup(c);
      if (gen != s_gen || !app_run) break;
      if (rc == CURLE_OK && code < 400) { s_eof_gen = gen; break; }       // termino de bajar
      if (x.pos > off) { off = x.pos; tries = 0; }                       // se corto: sigue donde quedo
      if (code >= 400 || ++tries > 5) { s_err = code >= 400 ? (int)code : -(int)rc; s_eof_gen = gen; break; }
      sceKernelDelayThread(1000000);
    }
  }
  return 0;
}

static int dec_thread(SceSize a, void *u) {
  static uint8_t ib[64 * 1024];
  static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  static int16_t obuf[2][MINIMP3_MAX_SAMPLES_PER_FRAME];
  static mp3dec_t mp3d;
  int ib_len = 0, my_gen = -1, port = -1, prate = 0, pch = 0, pgrain = 0, ob = 0, id3_done = 1;
  long long id3_skip = 0;
  while (app_run) {
    int gen = s_gen;
    if (gen != my_gen) {
      my_gen = gen; ib_len = 0; mp3dec_init(&mp3d);
      id3_done = s_off != 0; id3_skip = 0;           // la etiqueta ID3 solo esta al inicio del archivo
    }
    if (!p_active || p_paused || p_end_gen == gen) { sceKernelDelayThread(10000); continue; }
    plock();
    int n = 0;
    if (gen == s_gen) {
      int room = (int)sizeof ib - ib_len;
      n = r_cnt < room ? r_cnt : room;
      int first = RING_SZ - r_rd < n ? RING_SZ - r_rd : n;
      memcpy(ib + ib_len, ring + r_rd, first);
      if (n > first) memcpy(ib + ib_len + first, ring, n - first);
      r_rd = (r_rd + n) % RING_SZ; r_cnt -= n;
    }
    punlock();
    ib_len += n;
    int eof = s_eof_gen == gen;
    if (!id3_done) {                                  // salta la etiqueta ID3 (puede traer la portada adentro)
      if (id3_skip > 0) {
        int k = id3_skip < ib_len ? (int)id3_skip : ib_len;
        memmove(ib, ib + k, ib_len - k); ib_len -= k; id3_skip -= k;
        if (id3_skip > 0) { if (!n) sceKernelDelayThread(5000); continue; }
        id3_done = 1;
      } else if (ib_len >= 10) {
        if (!memcmp(ib, "ID3", 3)) {
          id3_skip = 10 + (((ib[6] & 0x7F) << 21) | ((ib[7] & 0x7F) << 14) | ((ib[8] & 0x7F) << 7) | (ib[9] & 0x7F)) + ((ib[5] & 0x10) ? 10 : 0);
          continue;
        }
        id3_done = 1;
      } else if (!eof) { sceKernelDelayThread(5000); continue; }
      else id3_done = 1;
    }
    if (ib_len < 16384 && !eof) { if (!n) sceKernelDelayThread(5000); continue; }   // junta datos para sincronizar
    if (ib_len == 0) { if (eof) p_end_gen = gen; continue; }
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&mp3d, ib, ib_len, pcm, &info);
    if (info.frame_bytes == 0) {                      // nada decodificable en el bufer
      if (eof) { ib_len = 0; p_end_gen = gen; }
      else if (ib_len >= (int)sizeof ib) { memmove(ib, ib + ib_len / 2, ib_len - ib_len / 2); ib_len -= ib_len / 2; }
      else sceKernelDelayThread(5000);
      continue;
    }
    if (info.frame_bytes > ib_len) info.frame_bytes = ib_len;
    memmove(ib, ib + info.frame_bytes, ib_len - info.frame_bytes);
    ib_len -= info.frame_bytes;
    if (samples <= 0 || gen != s_gen) continue;
    int ch = info.channels;
    if (ch < 1 || ch > 2 || samples % 64) continue;    // sceAudioOut pide bloques de 64 muestras
    p_rate = info.hz; p_kbps = info.bitrate_kbps;
    if (port < 0 || info.hz != prate || ch != pch || samples != pgrain) {
      if (port >= 0) sceAudioOutReleasePort(port);
      port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, samples, info.hz,
                                 ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
      if (port < 0) { s_err = port; port = -1; continue; }
      int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
      sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
      prate = info.hz; pch = ch; pgrain = samples;
    }
    memcpy(obuf[ob], pcm, (size_t)samples * ch * sizeof(int16_t));
    sceAudioOutOutput(port, obuf[ob]);
    ob ^= 1;
    if (gen == s_gen) p_samples += samples;
  }
  if (port >= 0) sceAudioOutReleasePort(port);
  return 0;
}

static void p_start(const char *url, long long off, double base) {
  plock();
  s_gen++;
  r_rd = r_wr = r_cnt = 0;
  snprintf(s_url, sizeof s_url, "%s", url);
  s_off = off; s_err = 0;
  p_base = base; p_samples = 0; p_paused = 0;
  punlock();
}
static void p_stop(void) {
  plock(); s_gen++; s_url[0] = 0; r_rd = r_wr = r_cnt = 0; punlock();
  p_active = 0;
}
static double p_pos(void) { return p_base + (p_rate ? (double)p_samples / p_rate : 0); }

static void fmt_time(double t, char *o, int cap) {
  if (t < 0) t = 0;
  int s = (int)t, h = s / 3600, m = (s / 60) % 60; s %= 60;
  if (h) snprintf(o, cap, "%d:%02d:%02d", h, m, s); else snprintf(o, cap, "%d:%02d", m, s);
}

static double track_len(const Track *t) {
  if (t->len > 0) return t->len;
  if (t->size > 0 && p_kbps > 0) return (double)t->size * 8 / (p_kbps * 1000.0);
  return 0;
}

static void track_url(const Track *t, char *o, int cap) {
  char ide[200], fe[600];
  url_enc(alb_id, ide, sizeof ide, 0);
  url_enc(t->file, fe, sizeof fe, 1);
  snprintf(o, cap, IA_DL, ide, fe);
}

static void play_track_at(int i, double t) {
  char url[1200];
  track_url(&tracks[i], url, sizeof url);
  double len = track_len(&tracks[i]);
  long long off = 0;
  if (t > 0 && len > 0 && tracks[i].size > 0) off = (long long)(tracks[i].size * (t / len));
  else if (t > 0 && p_kbps > 0) off = (long long)(t * p_kbps * 125);
  if (off <= 0) t = 0;
  p_start(url, off, t);
}

// Pantalla del reproductor. Devuelve LS_BACK o LS_EXIT.
static int player(int idx) {
  char cover[160]; snprintf(cover, sizeof cover, IA_IMG, alb_id);
  p_active = 1;
  play_track_at(idx, 0);
  int seek_pend = 0, ret = LS_BACK;
  double seek_to = 0;
  uint64_t seek_t = 0;
  char cut[200], l[200], a[16], b[16];
  for (;;) {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned pr = pad.buttons & ~oldpad.buttons; oldpad = pad;
    if (pr & SCE_CTRL_CIRCLE) break;
    if (pr & SCE_CTRL_START) { ret = LS_EXIT; break; }
    if (pr & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) p_paused = !p_paused;
    int step = 0;
    if (pr & SCE_CTRL_LEFT) step = -10;
    if (pr & SCE_CTRL_RIGHT) step = 10;
    if (pr & SCE_CTRL_DOWN) step = -60;
    if (pr & SCE_CTRL_UP) step = 60;
    double len = track_len(&tracks[idx]);
    if (step) {                                        // junta varios toques y salta una vez
      if (!seek_pend) { seek_to = p_pos(); seek_pend = 1; }
      seek_to += step;
      if (seek_to < 0) seek_to = 0;
      if (len > 0 && seek_to > len - 2) seek_to = len - 2;
      seek_t = now_us();
    }
    if (seek_pend && now_us() - seek_t > 600000) { play_track_at(idx, seek_to); seek_pend = 0; }
    int next = -1;
    if (pr & SCE_CTRL_RTRIGGER) next = idx + 1;
    if (pr & SCE_CTRL_LTRIGGER) next = p_pos() > 5 ? idx : idx - 1;   // como en cualquier reproductor
    if (!seek_pend && p_end_gen == s_gen) next = idx + 1;            // termino: la siguiente
    if (next >= 0 && next < ntracks) { idx = next; seek_pend = 0; play_track_at(idx, 0); }
    else if (next >= ntracks) { p_paused = 1; plock(); p_end_gen = s_gen; punlock(); }

    vita2d_wait_rendering_done();
    img_gc();
    vita2d_start_drawing(); vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(14,14,28,255));
    vita2d_draw_rectangle(30, 40, 340, 340, RGBA8(30,30,50,255));
    vita2d_texture *t = img_get(cover);
    if (t) draw_fit(t, 30, 40, 340, 340);
    else vita2d_pgf_draw_text(pgf, 120, 215, RGBA8(120,120,150,255), 1.0f, "Cargando portada...");
    vita2d_pgf_draw_text(pgf, 400, 70, RGBA8(170,170,200,255), 1.0f, fit_text(alb_creator, 1.0f, 530, cut, sizeof cut));
    vita2d_pgf_draw_text(pgf, 400, 100, RGBA8(200,200,230,255), 1.0f, fit_text(alb_title, 1.0f, 530, cut, sizeof cut));
    snprintf(l, sizeof l, "Pista %d de %d", idx + 1, ntracks);
    vita2d_pgf_draw_text(pgf, 400, 150, RGBA8(150,150,180,255), 0.9f, l);
    vita2d_pgf_draw_text(pgf, 400, 190, RGBA8(255,255,255,255), 1.4f, fit_text(tracks[idx].title, 1.4f, 530, cut, sizeof cut));
    // siguientes pistas
    for (int k = 1; k <= 4 && idx + k < ntracks; k++) {
      snprintf(l, sizeof l, "%d. %s", idx + k + 1, tracks[idx + k].title);
      vita2d_pgf_draw_text(pgf, 400, 230 + k * 28, RGBA8(140,140,170,255), 0.9f, fit_text(l, 0.9f, 530, cut, sizeof cut));
    }
    // barra de tiempo
    double pos = seek_pend ? seek_to : p_pos();
    double f = len > 0 ? pos / len : 0; if (f < 0) f = 0; if (f > 1) f = 1;
    vita2d_draw_rectangle(30, 420, 900, 8, RGBA8(70,70,90,255));
    vita2d_draw_rectangle(30, 420, (float)(900 * f), 8, seek_pend ? RGBA8(255,200,60,255) : RGBA8(90,160,255,255));
    fmt_time(pos, a, sizeof a); fmt_time(len, b, sizeof b);
    const char *state = p_paused ? "PAUSA" : seek_pend ? "Saltando..." :
                        (s_err && s_eof_gen == s_gen) ? "Error de red" :
                        (p_samples == 0) ? "Cargando..." : "";
    snprintf(l, sizeof l, "%s / %s   %s", a, len > 0 ? b : "--:--", state);
    vita2d_pgf_draw_text(pgf, 30, 456, RGBA8(255,255,255,255), 1.0f, l);
    if (s_err && s_eof_gen == s_gen) {
      snprintf(l, sizeof l, "codigo %d", s_err);
      vita2d_pgf_draw_text(pgf, 700, 456, RGBA8(255,120,120,255), 0.9f, l);
    }
    vita2d_pgf_draw_text(pgf, 30, 500, RGBA8(160,160,190,255), 0.85f,
                         "X: pausa   </>: 10 s   arriba/abajo: 1 min   L/R: pista anterior/siguiente   O: volver");
    vita2d_end_drawing(); vita2d_swap_buffers();
  }
  p_stop();
  return ret;
}

// =====================================================================
//                              NAVEGACION
// =====================================================================
// Album -> pistas -> reproductor. Devuelve 0 o LS_EXIT.
static int open_album(const char *id) {
  status("Cargando pistas...");
  if (load_tracks(id) < 0) { show_error("Este album no tiene MP3 para escuchar"); return 0; }
  EntryList l; memset(&l, 0, sizeof l);
  char cover[160]; snprintf(cover, sizeof cover, IA_IMG, id);
  for (int i = 0; i < ntracks; i++) {
    Entry *e = el_add(&l); if (!e) break;
    char tl[16]; double len = tracks[i].len;
    if (len > 0) fmt_time(len, tl, sizeof tl); else tl[0] = 0;
    snprintf(e->name, sizeof e->name, "%d. %s%s%s%s", i + 1, tracks[i].title, tl[0] ? "  (" : "", tl, tl[0] ? ")" : "");
    snprintf(e->key, sizeof e->key, "%s", tracks[i].title);
    snprintf(e->img, sizeof e->img, "%s", cover);
  }
  el_names(&l);
  ListState ls; memset(&ls, 0, sizeof ls);
  char title[200]; snprintf(title, sizeof title, "%s", alb_title[0] ? alb_title : id);
  int ret = 0;
  for (;;) {
    int r = list_screen_img(title, l.names, l.keys, l.imgs, 0, l.n, &ls, "X: escuchar desde esta pista   O: volver", 0);
    if (r == LS_EXIT) { ret = LS_EXIT; break; }
    if (r < 0) break;
    if (player(r) == LS_EXIT) { ret = LS_EXIT; break; }
  }
  el_free(&l);
  return ret;
}

// Lista de albumes. Devuelve 0 o LS_EXIT.
static int album_list(const char *title, EntryList *albums) {
  ListState ls; memset(&ls, 0, sizeof ls);
  for (;;) {
    int r = list_screen_img(title, albums->names, albums->keys, albums->imgs, 0, albums->n, &ls,
                            "X: abrir album   O: volver", 0);
    if (r == LS_EXIT) return LS_EXIT;
    if (r < 0) return 0;
    char id[96]; snprintf(id, sizeof id, "%s", albums->e[r].id);
    if (open_album(id) == LS_EXIT) return LS_EXIT;
  }
}

static int browse_artists(void) {
  static EntryList artists;
  static ListState ls;
  if (!artists.n) {
    status("Descargando artistas (puede tardar un poco)...");
    if (ia_search(Q_ARTISTS, "titleSorter asc", 10000, 0, &artists) < 0) { el_free(&artists); show_error("No se pudo bajar la lista de artistas"); return 0; }
  }
  for (;;) {
    int r = list_screen_img("Artistas (conciertos en vivo)", artists.names, artists.keys, artists.imgs, 0, artists.n, &ls,
                            "X: ver conciertos   O: menu", 0);
    if (r == LS_EXIT) return LS_EXIT;
    if (r < 0) return 0;
    char q[200]; snprintf(q, sizeof q, Q_ARTIST, artists.e[r].id);
    EntryList albums; memset(&albums, 0, sizeof albums);
    status("Descargando conciertos...");
    if (ia_search(q, "date desc", 1000, 0, &albums) < 0) { el_free(&albums); show_error("Sin conciertos"); continue; }
    char title[220]; snprintf(title, sizeof title, "%s", artists.e[r].name);
    int x = album_list(title, &albums);
    el_free(&albums);
    if (x == LS_EXIT) return LS_EXIT;
  }
}

static const char *GENRES[][2] = {
  { "Rock", "rock" }, { "Electronica", "electronic" }, { "Ambient", "ambient" }, { "Hip Hop", "hip hop" },
  { "Jazz", "jazz" }, { "Pop", "pop" }, { "Folk", "folk" }, { "Metal", "metal" }, { "Punk", "punk" },
  { "Reggae", "reggae" }, { "Dub", "dub" }, { "Blues", "blues" }, { "Clasica", "classical" },
  { "Techno", "techno" }, { "House", "house" }, { "Drum and bass", "drum and bass" },
  { "Downtempo", "downtempo" }, { "Lo-fi", "lo-fi" }, { "Chiptune", "chiptune" },
  { "Experimental", "experimental" }, { "Indie", "indie" }, { "Latina", "latin" }, { "Soundtrack", "soundtrack" },
};
#define NGENRES ((int)(sizeof GENRES / sizeof GENRES[0]))

static int browse_genres(void) {
  static ListState ls;
  const char *names[NGENRES];
  for (int i = 0; i < NGENRES; i++) names[i] = GENRES[i][0];
  for (;;) {
    int r = list_screen("Generos (musica libre de netlabels)", names, NGENRES, &ls, "X: ver albumes   O: menu");
    if (r == LS_EXIT) return LS_EXIT;
    if (r < 0) return 0;
    char q[200]; snprintf(q, sizeof q, Q_GENRE, GENRES[r][1]);
    EntryList albums; memset(&albums, 0, sizeof albums);
    status("Descargando albumes...");
    if (ia_search(q, "downloads desc", 500, 1, &albums) < 0) { el_free(&albums); show_error("Sin albumes en este genero"); continue; }
    char title[120]; snprintf(title, sizeof title, "Genero: %s", GENRES[r][0]);
    int x = album_list(title, &albums);
    el_free(&albums);
    if (x == LS_EXIT) return LS_EXIT;
  }
}

// Busqueda libre: deja solo letras, numeros y espacios (lo demas tiene significado en la consulta)
static int browse_search(void) {
  static char query[128];
  if (!ime_search_t("Buscar artista o album", query, sizeof query) || !query[0]) return 0;
  char clean[128]; int j = 0;
  for (int i = 0; query[i] && j < (int)sizeof clean - 1; i++) {
    unsigned char c = (unsigned char)query[i];
    clean[j++] = (isalnum(c) || c >= 0x80) ? c : ' ';
  }
  clean[j] = 0;
  char q[600]; snprintf(q, sizeof q, Q_SEARCH, clean, clean);
  EntryList res; memset(&res, 0, sizeof res);
  status("Buscando...");
  if (ia_search(q, "downloads desc", 300, 1, &res) < 0) { el_free(&res); show_error("Sin resultados"); return 0; }
  char title[200]; snprintf(title, sizeof title, "Resultados: %s", query);
  int x = album_list(title, &res);
  el_free(&res);
  return x;
}

// ---------- Main ----------
int main(void) {
  sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  static char netmem[1024 * 1024];
  SceNetInitParam np = { netmem, sizeof netmem, 0 };
  sceNetInit(&np); sceNetCtlInit();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  {
    SceAppUtilInitParam aip; SceAppUtilBootParam abp;
    memset(&aip, 0, sizeof aip); memset(&abp, 0, sizeof abp);
    sceAppUtilInit(&aip, &abp);                 // necesario para el teclado
  }
  vita2d_init(); pgf = vita2d_load_default_pgf();
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  ring = malloc(RING_SZ);
  p_mtx = sceKernelCreateMutex("mv_play", 0, 0, NULL);
  img_mtx = sceKernelCreateMutex("mv_img", 0, 0, NULL);
  img_run = 1;
  SceUID th_img = sceKernelCreateThread("mv_img", img_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
  SceUID th_net = sceKernelCreateThread("mv_net", stream_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
  SceUID th_dec = sceKernelCreateThread("mv_dec", dec_thread, 0x10000100, 128 * 1024, 0, 0, NULL);
  sceKernelStartThread(th_img, 0, NULL);
  sceKernelStartThread(th_net, 0, NULL);
  sceKernelStartThread(th_dec, 0, NULL);

  static const char *menu[] = { "Artistas (conciertos en vivo)", "Generos (musica libre)", "Buscar artista o album" };
  ListState ls; memset(&ls, 0, sizeof ls);
  for (;;) {
    int r = list_screen("Musica de Internet Archive", menu, 3, &ls, "X: entrar");
    if (r == LS_EXIT) break;
    int x = 0;
    if (r == 0) x = browse_artists();
    else if (r == 1) x = browse_genres();
    else if (r == 2) x = browse_search();
    if (x == LS_EXIT) break;
  }

  app_run = 0; img_run = 0; g_abort = 1;
  sceKernelWaitThreadEnd(th_img, NULL, NULL);
  sceKernelWaitThreadEnd(th_net, NULL, NULL);
  sceKernelWaitThreadEnd(th_dec, NULL, NULL);
  vita2d_fini();
  curl_global_cleanup(); sceNetCtlTerm(); sceNetTerm();
  sceKernelExitProcess(0);
  return 0;
}
