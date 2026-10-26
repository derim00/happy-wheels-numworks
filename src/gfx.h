// Strip based renderer: a frame is a z-sorted list of draw items rendered
// band by band (the full 320x240 frame buffer would not fit in RAM).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "phys.h"

#define SCR_W 320
#define SCR_H 240
#define STRIP_H 16

enum { DI_POLY, DI_CIRCLE, DI_SPRITE, DI_RECT, DI_LINE, DI_GRAD, DI_TILE, DI_HSPRITE };

typedef struct {
  uint8_t kind, alpha;          // alpha 0..32
  uint16_t color, color2;
  uint16_t z;
  int16_t y0, y1;               // rows covered (inclusive/exclusive)
  union {
    struct { const VI *v; uint16_t n; uint8_t outline, pad; float m[6]; } poly;
    struct { float cx, cy, r, ri; float lx, ly; } circ;      // lx,ly: radius line end (0 = none)
    struct { uint16_t id; uint8_t flip, pad; float x, y, c, s; float scale; } spr;
    struct { int16_t x, y, w, h; } rect;
    struct { float x0, y0, x1, y1, w; } line;
    struct { const uint16_t *g; } grad;
    struct { uint16_t id; int16_t ox, oy; } tile;
  };
} DrawItem;

typedef struct {
  float x, y;       // camera center (world meters)
  float zoom;       // pixels per meter
} Camera;

extern Camera cam;
extern uint16_t g_strip[SCR_W * STRIP_H];
extern int g_strip_y;

void gfx_begin(void);
DrawItem *gfx_add(int kind, int z);
void gfx_render(uint16_t bgcolor);
int gfx_count(void);

static inline float w2sx(float x) { return (x - cam.x) * cam.zoom + SCR_W / 2; }
static inline float w2sy(float y) { return (y - cam.y) * cam.zoom + SCR_H / 2; }

// helpers that add items
void draw_world_poly(const VI *v, int n, float ox, float oy, float c, float s, uint16_t color, uint8_t alpha, int z, bool outline, uint16_t oc);
void draw_world_circle(float x, float y, float r, float ri, uint16_t color, uint8_t alpha, int z, bool outline, uint16_t oc);
void draw_sprite_world(int id, float x, float y, float angle, float scale, bool flip, uint8_t alpha, int z);
void draw_sprite_screen(int id, float x, float y, float angle, float scale, bool flip, uint8_t alpha, int z);
void draw_rect(int x, int y, int w, int h, uint16_t color, uint8_t alpha, int z);
void draw_line_world(float x0, float y0, float x1, float y1, float width, uint16_t color, int z);
void draw_gradient(const uint16_t *g, int z);
void draw_tiled(int id, int ox, int oy, int z);
int draw_number(const char *txt, int x, int y, int z, float scale);  // timer font, returns width

// sprite metadata
typedef struct { uint16_t w, h; int16_t ax, ay; uint16_t pal, flags; uint32_t off; } SpriteDef;
extern const uint8_t *g_assets;
const SpriteDef *sprite_def(int id);
static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
