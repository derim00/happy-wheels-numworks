#include "gfx.h"
#include "platform.h"
#include "../build/assets.h"
#include <string.h>

#define MAX_ITEMS 300

Camera cam;
uint16_t g_strip[SCR_W * STRIP_H];
int g_strip_y;
const uint8_t *g_assets;

static DrawItem s_items[MAX_ITEMS];
static uint16_t s_order[MAX_ITEMS];
static int s_n;

const SpriteDef *sprite_def(int id) {
  return (const SpriteDef *)(g_assets + ASSET_SPR_OFF) + id;
}
static inline const uint32_t *sprite_pal(const SpriteDef *d) {
  return (const uint32_t *)(g_assets + ASSET_PAL_OFF) + d->pal * 256;
}

void gfx_begin(void) { s_n = 0; }
int gfx_count(void) { return s_n; }

DrawItem *gfx_add(int kind, int z) {
  if (s_n >= MAX_ITEMS) return 0;
  DrawItem *it = &s_items[s_n++];
  it->kind = kind; it->z = z; it->alpha = 32; it->y0 = 0; it->y1 = SCR_H;
  return it;
}

// ------------------------------------------------------------------ pixel ops
static inline uint16_t blend(uint16_t d, uint16_t s, uint32_t a) {
  uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81F;
  uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
  uint32_t r = ((dd * (32 - a) + ss * a) >> 5) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}

static void span(int y, int x0, int x1, uint16_t c, uint32_t a) {
  if (x0 < 0) x0 = 0;
  if (x1 > SCR_W) x1 = SCR_W;
  if (x0 >= x1) return;
  uint16_t *p = g_strip + (y - g_strip_y) * SCR_W + x0;
  int n = x1 - x0;
  if (a >= 32) { while (n--) *p++ = c; }
  else if (a > 0) { while (n--) { *p = blend(*p, c, a); p++; } }
}

// ------------------------------------------------------------------ polygons
typedef struct { float x, ydir, dxdy, ylo, yhi; } Edge;
#define MAX_EDGES 200
static Edge s_edges[MAX_EDGES];

static void fill_edges(int ne, uint16_t color, uint32_t a) {
  float xs[64];
  int ys = g_strip_y, ye = g_strip_y + STRIP_H;
  for (int y = ys; y < ye; y++) {
    float yc = y + 0.5f;
    int nx = 0;
    for (int i = 0; i < ne && nx < 64; i++) {
      Edge *e = &s_edges[i];
      if (yc >= e->ylo && yc < e->yhi) xs[nx++] = e->x + (yc - e->ylo) * e->dxdy;
    }
    if (nx < 2) continue;
    for (int i = 1; i < nx; i++) {   // insertion sort
      float v = xs[i]; int j = i - 1;
      while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
      xs[j + 1] = v;
    }
    for (int i = 0; i + 1 < nx; i += 2) {
      int x0 = (int)(xs[i] + 0.5f), x1 = (int)(xs[i + 1] + 0.5f);
      if (xs[i] < -1000) x0 = -1;
      span(y, x0, x1, color, a);
    }
  }
}

static int add_edge(int ne, float x0, float y0, float x1, float y1) {
  if (ne >= MAX_EDGES) return ne;
  float ylo, yhi, xlo;
  if (y0 == y1) return ne;
  if (y0 < y1) { ylo = y0; yhi = y1; xlo = x0; } else { ylo = y1; yhi = y0; xlo = x1; }
  if (yhi <= g_strip_y || ylo >= g_strip_y + STRIP_H) return ne;
  // clamp absurd x for stability
  Edge *e = &s_edges[ne];
  e->dxdy = (x1 - x0) / (y1 - y0);
  e->x = xlo; e->ylo = ylo; e->yhi = yhi;
  return ne + 1;
}

static void plot_line(float x0, float y0, float x1, float y1, uint16_t c) {
  // DDA clipped to strip
  float dx = x1 - x0, dy = y1 - y0;
  float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int steps = (int)(adx > ady ? adx : ady) + 1;
  if (steps > 2000) steps = 2000;
  float sx = dx / steps, sy = dy / steps;
  float x = x0, y = y0;
  for (int i = 0; i <= steps; i++) {
    int ix = (int)x, iy = (int)y;
    if (iy >= g_strip_y && iy < g_strip_y + STRIP_H && ix >= 0 && ix < SCR_W) g_strip[(iy - g_strip_y) * SCR_W + ix] = c;
    x += sx; y += sy;
  }
}

static void render_poly(const DrawItem *it) {
  const float *m = it->poly.m;
  const VI *v = it->poly.v;
  int n = it->poly.n;
  if (n < 2) return;
  float fx = m[0] * v[0].x + m[1] * v[0].y + m[2];
  float fy = m[3] * v[0].x + m[4] * v[0].y + m[5];
  float px = fx, py = fy;
  int ne = 0;
  bool ol = it->poly.outline;
  for (int i = 1; i <= n; i++) {
    float x, y;
    if (i == n) { x = fx; y = fy; }
    else { x = m[0] * v[i].x + m[1] * v[i].y + m[2]; y = m[3] * v[i].x + m[4] * v[i].y + m[5]; }
    ne = add_edge(ne, px, py, x, y);
    px = x; py = y;
  }
  fill_edges(ne, it->color, it->alpha);
  if (ol) {
    px = fx; py = fy;
    for (int i = 1; i <= n; i++) {
      float x, y;
      if (i == n) { x = fx; y = fy; }
      else { x = m[0] * v[i].x + m[1] * v[i].y + m[2]; y = m[3] * v[i].x + m[4] * v[i].y + m[5]; }
      if (!((py < g_strip_y && y < g_strip_y) || (py >= g_strip_y + STRIP_H && y >= g_strip_y + STRIP_H)))
        plot_line(px, py, x, y, it->color2);
      px = x; py = y;
    }
  }
}

static void render_line(const DrawItem *it) {
  float dx = it->line.x1 - it->line.x0, dy = it->line.y1 - it->line.y0;
  float l = sqrtf(dx * dx + dy * dy);
  if (l < 0.01f) return;
  float nx = -dy / l * it->line.w * 0.5f, ny = dx / l * it->line.w * 0.5f;
  float qx[4] = { it->line.x0 + nx, it->line.x1 + nx, it->line.x1 - nx, it->line.x0 - nx };
  float qy[4] = { it->line.y0 + ny, it->line.y1 + ny, it->line.y1 - ny, it->line.y0 - ny };
  int ne = 0;
  for (int i = 0; i < 4; i++) ne = add_edge(ne, qx[i], qy[i], qx[(i + 1) & 3], qy[(i + 1) & 3]);
  fill_edges(ne, it->color, it->alpha);
}

static void render_circle(const DrawItem *it) {
  float cx = it->circ.cx, cy = it->circ.cy, r = it->circ.r, ri = it->circ.ri;
  int ys = g_strip_y, ye = g_strip_y + STRIP_H;
  for (int y = ys; y < ye; y++) {
    float dy = y + 0.5f - cy;
    if (dy * dy >= r * r) continue;
    float h = sqrtf(r * r - dy * dy);
    if (ri > 0 && dy * dy < ri * ri) {
      float hi = sqrtf(ri * ri - dy * dy);
      span(y, (int)(cx - h + 0.5f), (int)(cx - hi + 0.5f), it->color, it->alpha);
      span(y, (int)(cx + hi + 0.5f), (int)(cx + h + 0.5f), it->color, it->alpha);
    } else span(y, (int)(cx - h + 0.5f), (int)(cx + h + 0.5f), it->color, it->alpha);
  }
  if (it->color2 != it->color) {
    // outline ring
    int steps = (int)(r * 6.3f) + 8;
    for (int i = 0; i < steps; i++) {
      float a = i * 6.2831853f / steps;
      int x = (int)(cx + cosf(a) * (r - 0.5f)), y = (int)(cy + sinf(a) * (r - 0.5f));
      if (y >= ys && y < ye && x >= 0 && x < SCR_W) g_strip[(y - ys) * SCR_W + x] = it->color2;
    }
  }
}

// ------------------------------------------------------------------ sprites
static void render_sprite(const DrawItem *it) {
  const SpriteDef *d = sprite_def(it->spr.id);
  const uint32_t *pal = sprite_pal(d);
  const uint8_t *px = g_assets + d->off;
  float ax = d->ax * 0.25f, ay = d->ay * 0.25f;
  float c = it->spr.c, s = it->spr.s, sc = it->spr.scale;
  float fl = it->spr.flip ? -1.0f : 1.0f;
  float X = it->spr.x, Y = it->spr.y;
  int W = d->w, H = d->h;
  uint32_t ga = it->alpha;
  int ys = g_strip_y > it->y0 ? g_strip_y : it->y0;
  int ye = g_strip_y + STRIP_H < it->y1 ? g_strip_y + STRIP_H : it->y1;
  if (s == 0 && c == 1 && sc == 1 && !it->spr.flip) {
    // fast path
    int x0 = (int)floorf(X - ax + 0.5f), y0 = (int)floorf(Y - ay + 0.5f);
    for (int y = ys; y < ye; y++) {
      int v = y - y0;
      if (v < 0 || v >= H) continue;
      const uint8_t *row = px + v * W;
      uint16_t *dst = g_strip + (y - g_strip_y) * SCR_W;
      int u0 = 0, u1 = W;
      if (x0 + u0 < 0) u0 = -x0;
      if (x0 + u1 > SCR_W) u1 = SCR_W - x0;
      for (int u = u0; u < u1; u++) {
        uint8_t i = row[u];
        if (!i) continue;
        uint32_t e = pal[i];
        uint32_t a = ((e >> 16) * ga) >> 8;
        uint16_t *p = dst + x0 + u;
        if (a >= 31) *p = (uint16_t)e; else *p = blend(*p, (uint16_t)e, a);
      }
    }
    return;
  }
  float isc = 1.0f / sc;
  // inverse map: u = fl*( c*(qx-X) + s*(qy-Y))/sc + ax ; v = (-s*(qx-X) + c*(qy-Y))/sc + ay
  float dudx = fl * c * isc, dvdx = -s * isc;
  for (int y = ys; y < ye; y++) {
    float qy = y + 0.5f - Y;
    // find x range where (u,v) inside: solve conservatively using bbox
    float x0f = 0, x1f = SCR_W;
    // u(x) = u0 + dudx*x ; v(x) = v0 + dvdx*x with qx = x + 0.5 - X
    float u0 = fl * (c * (0.5f - X) + s * qy) * isc + ax;
    float v0 = (-s * (0.5f - X) + c * qy) * isc + ay;
    // clip interval for u in [0,W) and v in [0,H)
    if (dudx != 0) {
      float a = (0 - u0) / dudx, b = (W - u0) / dudx;
      if (a > b) { float t = a; a = b; b = t; }
      if (a > x0f) x0f = a;
      if (b < x1f) x1f = b;
    } else if (u0 < 0 || u0 >= W) continue;
    if (dvdx != 0) {
      float a = (0 - v0) / dvdx, b = (H - v0) / dvdx;
      if (a > b) { float t = a; a = b; b = t; }
      if (a > x0f) x0f = a;
      if (b < x1f) x1f = b;
    } else if (v0 < 0 || v0 >= H) continue;
    int xa = (int)floorf(x0f), xb = (int)ceilf(x1f);
    if (xa < 0) xa = 0;
    if (xb > SCR_W) xb = SCR_W;
    uint16_t *dst = g_strip + (y - g_strip_y) * SCR_W;
    float u = u0 + dudx * xa, v = v0 + dvdx * xa;
    for (int x = xa; x < xb; x++, u += dudx, v += dvdx) {
      int iu = (int)u, iv = (int)v;
      if (u < 0 || v < 0 || iu >= W || iv >= H) continue;
      uint8_t i = px[iv * W + iu];
      if (!i) continue;
      uint32_t e = pal[i];
      uint32_t a = ((e >> 16) * ga) >> 8;
      if (a >= 31) dst[x] = (uint16_t)e; else dst[x] = blend(dst[x], (uint16_t)e, a);
    }
  }
}

static void render_tile(const DrawItem *it) {
  const SpriteDef *d = sprite_def(it->tile.id);
  const uint32_t *pal = sprite_pal(d);
  const uint8_t *px = g_assets + d->off;
  int W = d->w, H = d->h;
  for (int y = g_strip_y; y < g_strip_y + STRIP_H; y++) {
    int v = (y + it->tile.oy) % H; if (v < 0) v += H;
    const uint8_t *row = px + v * W;
    uint16_t *dst = g_strip + (y - g_strip_y) * SCR_W;
    int u = it->tile.ox % W; if (u < 0) u += W;
    for (int x = 0; x < SCR_W; x++) {
      uint8_t i = row[u];
      if (i) {
        uint32_t e = pal[i];
        uint32_t a = ((e >> 16) * it->alpha) >> 8;
        dst[x] = a >= 31 ? (uint16_t)e : blend(dst[x], (uint16_t)e, a);
      }
      if (++u == W) u = 0;
    }
  }
}

// ------------------------------------------------------------------ frame
void gfx_render(uint16_t bg) {
  // stable sort by z
  for (int i = 0; i < s_n; i++) s_order[i] = i;
  for (int i = 1; i < s_n; i++) {
    uint16_t k = s_order[i]; uint16_t kz = s_items[k].z;
    int j = i - 1;
    while (j >= 0 && s_items[s_order[j]].z > kz) { s_order[j + 1] = s_order[j]; j--; }
    s_order[j + 1] = k;
  }
  for (g_strip_y = 0; g_strip_y < SCR_H; g_strip_y += STRIP_H) {
    for (int i = 0; i < SCR_W * STRIP_H; i++) g_strip[i] = bg;
    for (int k = 0; k < s_n; k++) {
      const DrawItem *it = &s_items[s_order[k]];
      if (it->y1 <= g_strip_y || it->y0 >= g_strip_y + STRIP_H) continue;
      switch (it->kind) {
        case DI_POLY: render_poly(it); break;
        case DI_CIRCLE: render_circle(it); break;
        case DI_SPRITE: render_sprite(it); break;
        case DI_RECT:
          for (int y = g_strip_y; y < g_strip_y + STRIP_H; y++)
            if (y >= it->rect.y && y < it->rect.y + it->rect.h) span(y, it->rect.x, it->rect.x + it->rect.w, it->color, it->alpha);
          break;
        case DI_LINE: render_line(it); break;
        case DI_GRAD:
          for (int y = g_strip_y; y < g_strip_y + STRIP_H; y++) span(y, 0, SCR_W, it->grad.g[y], 32);
          break;
        case DI_TILE: render_tile(it); break;
      }
    }
    plat_push_strip(g_strip_y, g_strip);
  }
}

// ------------------------------------------------------------------ helpers
void draw_world_poly(const VI *v, int n, float ox, float oy, float c, float s, uint16_t color, uint8_t alpha, int z, bool outline, uint16_t oc) {
  // verts in VI units relative to (ox,oy) rotated by (c,s)
  float k = cam.zoom * VI_SCALE;
  // quick bbox cull
  float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
  float m0 = c * k, m1 = -s * k, m2 = w2sx(ox), m3 = s * k, m4 = c * k, m5 = w2sy(oy);
  int step = n > 64 ? n / 32 : 1;
  for (int i = 0; i < n; i += step) {
    float x = m0 * v[i].x + m1 * v[i].y + m2, y = m3 * v[i].x + m4 * v[i].y + m5;
    if (x < minx) minx = x;
    if (x > maxx) maxx = x;
    if (y < miny) miny = y;
    if (y > maxy) maxy = y;
  }
  if (step > 1) { float pad = 200; minx -= pad; maxx += pad; miny -= pad; maxy += pad; }
  if (maxx < 0 || minx > SCR_W || maxy < 0 || miny > SCR_H) return;
  DrawItem *it = gfx_add(DI_POLY, z);
  if (!it) return;
  it->color = color; it->color2 = oc; it->alpha = alpha;
  it->poly.v = v; it->poly.n = n; it->poly.outline = outline;
  it->poly.m[0] = m0; it->poly.m[1] = m1; it->poly.m[2] = m2;
  it->poly.m[3] = m3; it->poly.m[4] = m4; it->poly.m[5] = m5;
  it->y0 = (int16_t)clampf(miny - 1, 0, SCR_H); it->y1 = (int16_t)clampf(maxy + 2, 0, SCR_H);
}

void draw_world_circle(float x, float y, float r, float ri, uint16_t color, uint8_t alpha, int z, bool outline, uint16_t oc) {
  float sx = w2sx(x), sy = w2sy(y), sr = r * cam.zoom;
  if (sx + sr < 0 || sx - sr > SCR_W || sy + sr < 0 || sy - sr > SCR_H) return;
  DrawItem *it = gfx_add(DI_CIRCLE, z);
  if (!it) return;
  it->color = color; it->color2 = outline ? oc : color; it->alpha = alpha;
  it->circ.cx = sx; it->circ.cy = sy; it->circ.r = sr; it->circ.ri = ri * cam.zoom;
  it->y0 = (int16_t)clampf(sy - sr - 1, 0, SCR_H); it->y1 = (int16_t)clampf(sy + sr + 2, 0, SCR_H);
}

void draw_sprite_screen(int id, float x, float y, float angle, float scale, bool flip, uint8_t alpha, int z) {
  if (id < 0) return;
  const SpriteDef *d = sprite_def(id);
  float rad = 0.5f * scale * sqrtf((float)(d->w * d->w + d->h * d->h)) + scale * (fabsf(d->ax * 0.25f - d->w * 0.5f) + fabsf(d->ay * 0.25f - d->h * 0.5f));
  if (x + rad < 0 || x - rad > SCR_W || y + rad < 0 || y - rad > SCR_H) return;
  DrawItem *it = gfx_add(DI_SPRITE, z);
  if (!it) return;
  it->alpha = alpha;
  it->spr.id = id; it->spr.flip = flip;
  it->spr.x = x; it->spr.y = y;
  if (angle == 0) { it->spr.c = 1; it->spr.s = 0; }
  else { it->spr.c = cosf(angle); it->spr.s = sinf(angle); }
  it->spr.scale = scale;
  it->y0 = (int16_t)clampf(y - rad - 1, 0, SCR_H); it->y1 = (int16_t)clampf(y + rad + 2, 0, SCR_H);
}

void draw_sprite_world(int id, float x, float y, float angle, float scale, bool flip, uint8_t alpha, int z) {
  draw_sprite_screen(id, w2sx(x), w2sy(y), angle, scale, flip, alpha, z);
}

void draw_rect(int x, int y, int w, int h, uint16_t color, uint8_t alpha, int z) {
  DrawItem *it = gfx_add(DI_RECT, z);
  if (!it) return;
  it->color = color; it->alpha = alpha;
  it->rect.x = x; it->rect.y = y; it->rect.w = w; it->rect.h = h;
  it->y0 = (int16_t)clampf(y, 0, SCR_H); it->y1 = (int16_t)clampf(y + h, 0, SCR_H);
}

void draw_line_world(float x0, float y0, float x1, float y1, float width, uint16_t color, int z) {
  DrawItem *it = gfx_add(DI_LINE, z);
  if (!it) return;
  it->color = color;
  it->line.x0 = w2sx(x0); it->line.y0 = w2sy(y0); it->line.x1 = w2sx(x1); it->line.y1 = w2sy(y1);
  it->line.w = width;
  float a = it->line.y0 < it->line.y1 ? it->line.y0 : it->line.y1;
  float b = it->line.y0 < it->line.y1 ? it->line.y1 : it->line.y0;
  it->y0 = (int16_t)clampf(a - width, 0, SCR_H); it->y1 = (int16_t)clampf(b + width + 1, 0, SCR_H);
}

void draw_gradient(const uint16_t *g, int z) {
  DrawItem *it = gfx_add(DI_GRAD, z);
  if (it) it->grad.g = g;
}

void draw_tiled(int id, int ox, int oy, int z) {
  DrawItem *it = gfx_add(DI_TILE, z);
  if (it) { it->tile.id = id; it->tile.ox = ox; it->tile.oy = oy; }
}

int draw_number(const char *txt, int x, int y, int z, float scale) {
  int x0 = x;
  for (const char *p = txt; *p; p++) {
    int ch = (unsigned char)*p;
    int id = -1;
    switch (ch) {
      case '0': id = SPR_font_48; break; case '1': id = SPR_font_49; break; case '2': id = SPR_font_50; break;
      case '3': id = SPR_font_51; break; case '4': id = SPR_font_52; break; case '5': id = SPR_font_53; break;
      case '6': id = SPR_font_54; break; case '7': id = SPR_font_55; break; case '8': id = SPR_font_56; break;
      case '9': id = SPR_font_57; break; case ':': id = SPR_font_58; break; case '.': id = SPR_font_46; break;
      case '+': id = SPR_font_43; break;
    }
    if (id >= 0) draw_sprite_screen(id, x, y, 0, scale, false, 32, z);
    x += (int)(FONT_ADV[ch & 127] * scale);
  }
  return x - x0;
}
