// Host simulator: renders frames into a framebuffer, dumps PPM screenshots,
// keys come from a script: env HW_SCRIPT="frame:keymask,frame:keymask,..." (hex masks)
#include "platform.h"
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t fb[SCR_W * SCR_H];
static int frame;
static int dump_every = 0, max_frames = 600;
static const char *outdir = "shots";
static uint8_t *assets;
static struct { int f; uint64_t k; } script[512];
static int nscript;

void plat_init(void) {
  FILE *f = fopen(getenv("HW_ASSETS") ? getenv("HW_ASSETS") : "build/assets.bin", "rb");
  if (!f) { perror("assets"); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  assets = malloc(n); if (fread(assets, 1, n, f) != (size_t)n) exit(1); fclose(f);
  g_assets = assets;
  if (getenv("HW_DUMP")) dump_every = atoi(getenv("HW_DUMP"));
  if (getenv("HW_FRAMES")) max_frames = atoi(getenv("HW_FRAMES"));
  if (getenv("HW_OUT")) outdir = getenv("HW_OUT");
  const char *s = getenv("HW_SCRIPT");
  while (s && *s && nscript < 512) {
    int fr; unsigned long long k; int used;
    if (sscanf(s, "%d:%llx%n", &fr, &k, &used) != 2) break;
    script[nscript].f = fr; script[nscript].k = k; nscript++;
    s += used; if (*s == ',') s++;
  }
}

static void dump(const char *name) {
  char path[256]; snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
  FILE *f = fopen(path, "wb"); if (!f) return;
  fprintf(f, "P6 %d %d 255\n", SCR_W, SCR_H);
  for (int i = 0; i < SCR_W * SCR_H; i++) {
    uint16_t c = fb[i];
    unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

void plat_push_strip(int y, const uint16_t *pixels) {
  int h = STRIP_H; if (y + h > SCR_H) h = SCR_H - y;
  memcpy(fb + y * SCR_W, pixels, (size_t)SCR_W * h * 2);
  if (y + STRIP_H >= SCR_H) {
    frame++;
    if (dump_every && frame % dump_every == 0) { char n[32]; snprintf(n, sizeof n, "f%05d", frame); dump(n); }
  }
}

uint64_t plat_keys(void) {
  uint64_t k = 0;
  for (int i = 0; i < nscript; i++) if (script[i].f <= frame) k = script[i].k;
  return k;
}
uint32_t plat_millis(void) { return (uint32_t)(frame * 1000 / 30); }
void plat_sleep(uint32_t ms) { (void)ms; }
uint32_t plat_random(void) { return (uint32_t)rand(); }
bool plat_should_quit(void) { return frame >= max_frames; }

void *plat_arena(int *size) {
  static char *a; int s = getenv("HW_ARENA") ? atoi(getenv("HW_ARENA")) : 180 * 1024;
  if (!a) a = malloc(s);
  *size = s; return a;
}
