#include "level.h"
#include "gfx.h"
#include "game.h"
#include "../build/assets.h"
#include <string.h>

Level L;

static inline float vi2f(int v) { return v * VI_SCALE; }

int level_ref_body(uint16_t ref) {
  int kind = ref >> 14, idx = ref & 0x3FFF;
  switch (kind) {
    case 1:
      if (idx >= L.h->nTop) return 0;
      { uint16_t r = L.remap[idx]; if (r == 0xFFFF) return 0; return L.shapeBody[r] < 0 ? 0 : L.shapeBody[r]; }
    case 2: if (idx >= L.h->nGroups) return 0; return L.groupBody[idx] < 0 ? 0 : L.groupBody[idx];
    case 3: return special_body(idx);
    default: return 0;
  }
}

static void make_shape_fixture(int b, const LvShape *s, float ox, float oy, float a, bool fixed, uint16_t tagUser) {
  if (s->type == ST_ART || s->type == ST_TERR || !(s->flags & SF_INTER)) return;
  int f;
  float dens = s->dens / 100.0f;
  if (dens <= 0) dens = 0.1f;
  if (s->type == ST_CIRC) f = fix_circle(b, ox, oy, vi2f(s->r), dens);
  else {
    if (s->nverts < 3) return;
    f = fix_poly(b, L.verts + s->vofs, s->nverts, ox, oy, a, dens);
  }
  if (f < 0) return;
  uint16_t cat, mask; int16_t grp;
  static_filter(s->coll, fixed, &cat, &mask, &grp);
  fix_filter(f, cat, mask, grp);
  W.fix[f].tag = TAG_SHAPE; W.fix[f].user = tagUser;
  if (s->coll == 3) W.fix[f].flags |= FXF_SENSOR;
}

bool level_load(int idx, void *(*alloc)(int)) {
  memset(&L, 0, sizeof(L));
  L.level = idx;
  L.base = g_assets + LEVELS[idx].off;
  L.h = (const LvHeader *)L.base;
  const LvHeader *h = L.h;
  L.verts = (const VI *)(L.base + h->o_vp);
  L.remap = (const uint16_t *)(L.base + h->o_remap);
  L.shapes = (const LvShape *)(L.base + h->o_shp);
  L.groups = (const LvGroup *)(L.base + h->o_grp);
  L.joints = (const LvJoint *)(L.base + h->o_jnt);
  L.specials = (const LvSpecial *)(L.base + h->o_spc);
  L.triggers = (const LvTrigger *)(L.base + h->o_trg);
  L.targets = (const LvTarget *)(L.base + h->o_tgt);
  L.sdraw = (const LvSDraw *)(L.base + h->o_sd);
  L.dcol = (const uint32_t *)(L.base + h->o_dcol);
  L.dcolidx = (const uint16_t *)(L.base + h->o_dcolidx);

  // count top-level body shapes
  int nTopBodies = 0;
  for (int i = 0; i < h->nTop; i++) if (L.remap[i] != 0xFFFF) nTopBodies++;
  int nfix = 0;
  for (int i = 0; i < h->nBodyShapes; i++)
    if (L.shapes[i].type != ST_ART && L.shapes[i].type != ST_TERR && (L.shapes[i].flags & SF_INTER)) nfix++;
  int ngroups = 0;
  for (int g = 0; g < h->nGroups; g++) if (!(L.groups[g].flags & 0x80)) ngroups++;
  int nsp = h->nSpecials < MAX_SPECIALS ? h->nSpecials : MAX_SPECIALS;
  // player rig ~40 bodies/fixtures/joints, projectiles 12, impalements 12
  int maxb = 1 + nTopBodies + ngroups + nsp + 56;
  int maxf = nfix + nsp + 60;
  int maxj = h->nJoints + nsp + 64;
  int maxc = 100;
  phys_init(alloc, maxb, maxf, maxj, maxc);
  L.shapeBody = alloc(2 * (h->nBodyShapes + 1));
  L.groupBody = alloc(2 * (h->nGroups + 1));
  L.jointMap = alloc(2 * (h->nJoints + 1));
  L.bdraw = alloc(sizeof(BodyDraw) * maxb);
  L.shapeAlpha = alloc(h->nBodyShapes + 1);
  L.sp = alloc(sizeof(Special) * (nsp + 1));
  L.trig = alloc(sizeof(TrigRT) * (h->nTriggers + 1));
  if (!L.shapeBody || !L.groupBody || !L.jointMap || !L.bdraw || !L.shapeAlpha || !L.sp || !L.trig || !W.contacts || !W.order ||
      !W.bodies || !W.fix || !W.joints || !W.hash) return false;
  memset(L.bdraw, 0, sizeof(BodyDraw) * maxb);
  for (int i = 0; i < h->nBodyShapes; i++) { L.shapeBody[i] = -1; L.shapeAlpha[i] = L.shapes[i].opac; }

  // static world
  W.sw.prims = (const StaticPrim *)(L.base + h->o_sc);
  W.sw.verts = L.verts;
  W.sw.colidx = (const uint16_t *)(L.base + h->o_colidx);
  W.sw.cols = (const uint32_t *)(L.base + h->o_col);
  W.sw.ncol = h->nCol;
  W.sw.x0 = h->bx0; W.sw.colw = h->colw;
  W.sw.nprims = h->nSColl;
  L.bodyStart = W.nb;

  // top level body shapes
  int bi = 0;
  for (int i = 0; i < h->nTop; i++) {
    if (L.remap[i] == 0xFFFF) continue;
    int si = L.remap[i];
    const LvShape *s = &L.shapes[si];
    bool fixed = (s->flags & SF_FIXED) || !(s->flags & SF_INTER) || s->type == ST_ART || s->type == ST_TERR;
    int b = body_create(fixed ? BT_STATIC : BT_DYNAMIC, vi2f(s->x), vi2f(s->y), s->a / 8192.0f);
    if (b < 0) break;
    make_shape_fixture(b, s, 0, 0, 0, fixed, si);
    body_finalize(b, 0);
    W.bodies[b].tag = TAG_SHAPE; W.bodies[b].user = si;
    if (s->flags & SF_SLEEP) W.bodies[b].flags &= ~BF_AWAKE;
    L.bdraw[b].first = si; L.bdraw[b].n = 1; L.bdraw[b].opac = 100; L.bdraw[b].kind = 0;
    L.shapeBody[si] = b;
    bi++;
  }
  // groups
  for (int g = 0; g < h->nGroups; g++) {
    const LvGroup *gr = &L.groups[g];
    if (gr->flags & 0x80) { L.groupBody[g] = -1; continue; }   // baked into the static world
    bool fixed = gr->flags & 1;
    bool immov = gr->flags & 4;
    bool solid = false;
    for (int k = 0; k < gr->n; k++) {
      const LvShape *s = &L.shapes[gr->first + k];
      if ((s->flags & SF_INTER) && s->type != ST_ART && s->type != ST_TERR) solid = true;
    }
    if (!solid) fixed = true;   // pure decoration group
    int b = body_create(fixed ? BT_STATIC : BT_DYNAMIC, gr->x, gr->y, gr->a);
    L.groupBody[g] = b;
    if (b < 0) continue;
    if (gr->flags & 8) W.bodies[b].flags |= BF_FIXEDROT;
    for (int k = 0; k < gr->n; k++) {
      int si = gr->first + k;
      const LvShape *s = &L.shapes[si];
      make_shape_fixture(b, s, vi2f(s->x), vi2f(s->y), s->a / 8192.0f, fixed || immov, si);
      L.shapeBody[si] = b;
    }
    body_finalize(b, 0);
    W.bodies[b].tag = TAG_GROUP; W.bodies[b].user = g;
    if (gr->flags & 2) W.bodies[b].flags &= ~BF_AWAKE;
    L.bdraw[b].first = gr->first; L.bdraw[b].n = gr->n; L.bdraw[b].opac = gr->opac; L.bdraw[b].kind = 1;
  }
  // specials (may create bodies, and may live inside groups)
  L.nsp = 0;
  for (int i = 0; i < nsp; i++) special_create(i);
  // joints
  for (int i = 0; i < h->nJoints; i++) {
    const LvJoint *j = &L.joints[i];
    int b1 = level_ref_body(j->b1), b2 = level_ref_body(j->b2);
    L.jointMap[i] = -1;
    if (b1 == b2) continue;
    if (b1 == 0 && b2 == 0) continue;
    if (b1 == 0) { int t = b1; b1 = b2; b2 = t; }
    int jj;
    if (j->t == 0)
      jj = joint_revolute(b1, b2, j->x, j->y, j->fl & 1, j->lo, j->hi, j->fl & 2, j->spd, j->tq, j->fl & 4);
    else
      jj = joint_prismatic(b1, b2, j->x, j->y, cosf(j->ax), sinf(j->ax), j->fl & 1, j->lo, j->hi, j->fl & 2, j->spd, j->tq, j->fl & 4);
    L.jointMap[i] = jj;
    if (jj >= 0) { W.joints[jj].tag = TAG_SHAPE; W.joints[jj].user = i; }
  }
  // triggers
  for (int i = 0; i < h->nTriggers; i++) {
    TrigRT *t = &L.trig[i];
    memset(t, 0, sizeof(*t));
    t->enabled = !L.triggers[i].sd;
  }
  return true;
}

// ------------------------------------------------------------------ drawing
static void draw_shape_rec(const LvShape *s, float ox, float oy, float c, float sn, float ang, int alpha100) {
  if (alpha100 <= 0) return;
  uint8_t a = (uint8_t)((alpha100 * 32 + 50) / 100);
  if (a == 0) return;
  float sx = vi2f(s->x), sy = vi2f(s->y);
  float wx = ox + c * sx - sn * sy, wy = oy + sn * sx + c * sy;
  bool ol = s->flags & SF_OUTLINE;
  if (s->type == ST_CIRC) {
    float r = vi2f(s->r);
    draw_world_circle(wx, wy, r, r * s->cut / 100.0f, s->color, a, s->z + Z_LEVEL, ol, s->outline);
  } else {
    float sa = ang + s->a / 8192.0f;
    draw_world_poly(L.verts + s->vofs, s->nverts, wx, wy, cosf(sa), sinf(sa), s->color, a, s->z + Z_LEVEL, ol, s->outline);
  }
}

void level_draw(void) {
  const LvHeader *h = L.h;
  // static shapes: visible columns
  float wx0 = cam.x - SCR_W / 2 / cam.zoom, wx1 = cam.x + SCR_W / 2 / cam.zoom;
  float wy0 = cam.y - SCR_H / 2 / cam.zoom, wy1 = cam.y + SCR_H / 2 / cam.zoom;
  int c0 = (int)floorf((wx0 - h->bx0) / h->colw), c1 = (int)floorf((wx1 - h->bx0) / h->colw);
  if (c0 < 0) c0 = 0;
  if (c1 >= h->nCol) c1 = h->nCol - 1;
  int16_t qx0 = (int16_t)clampf(wx0 * 64, -32768, 32767), qx1 = (int16_t)clampf(wx1 * 64, -32768, 32767);
  int16_t qy0 = (int16_t)clampf(wy0 * 64, -32768, 32767), qy1 = (int16_t)clampf(wy1 * 64, -32768, 32767);
  for (int c = c0; c <= c1; c++) {
    uint32_t start = L.dcol[c * 2], cnt = L.dcol[c * 2 + 1] & 0xFFFF;
    for (uint32_t k = 0; k < cnt; k++) {
      const LvSDraw *d = &L.sdraw[L.dcolidx[start + k]];
      if (d->aabb[0] > qx1 || d->aabb[2] < qx0 || d->aabb[1] > qy1 || d->aabb[3] < qy0) continue;
      int pc = (int)floorf((d->aabb[0] * VI_SCALE - h->bx0) / h->colw);
      if (pc < c0) pc = c0;
      if (pc != c) continue;
      uint8_t a = (uint8_t)((d->opac * 32 + 50) / 100);
      if (d->kind == 1) {
        float r = d->r * VI_SCALE;
        draw_world_circle(d->x * VI_SCALE, d->y * VI_SCALE, r, r * d->cut / 100.0f, d->color, a, d->z + Z_LEVEL, d->outline, d->oc);
      } else {
        float ang = d->a / 8192.0f;
        draw_world_poly(L.verts + d->vofs, d->n, d->x * VI_SCALE, d->y * VI_SCALE, cosf(ang), sinf(ang), d->color, a, d->z + Z_LEVEL, d->outline, d->oc);
      }
    }
  }
  // body shapes
  for (int b = L.bodyStart; b < W.nb; b++) {
    const Body *bd = &W.bodies[b];
    if (bd->flags & BF_DEAD) continue;
    const BodyDraw *dr = &L.bdraw[b];
    if (!dr->n) continue;
    if (bd->cx < wx0 - 30 || bd->cx > wx1 + 30 || bd->cy < wy0 - 30 || bd->cy > wy1 + 30) continue;
    float ox, oy; body_origin(bd, &ox, &oy);
    for (int k = 0; k < dr->n; k++) {
      int si = dr->first + k;
      draw_shape_rec(&L.shapes[si], ox, oy, bd->c, bd->s, bd->a, L.shapeAlpha[si] * dr->opac / 100);
    }
  }
}

// ------------------------------------------------------------------ background
uint16_t level_bg_color(void) {
  int bg = L.h->bg;
  if (bg == 0) return L.h->bgc;
  return 0;
}

static int16_t s_bgdummy;

void level_draw_background(void) {
  int bg = L.h->bg;
  const uint16_t *grad = 0;
  (void)s_bgdummy;
  float px = cam.x * cam.zoom, py = cam.y * cam.zoom;
  switch (bg) {
    case 1: case 4001:
      grad = (const uint16_t *)(g_assets + ASSET_GRAD_BLUE);
      draw_gradient(grad, 0);
      {
        // parallax hills, near ones move faster
        static const int ids[3] = { SPR_greenSource3, SPR_greenSource2, SPR_greenSource1 };
        static const float fac[3] = { 0.04f, 0.08f, 0.14f };
        static const int yoff[3] = { 150, 175, 205 };
        for (int k = 0; k < 3; k++) {
          const SpriteDef *d = sprite_def(ids[k]);
          int w = d->w;
          float off = -px * fac[k];
          int x = (int)off % w; if (x > 0) x -= w;
          int y = yoff[k] - (int)(py * fac[k] * 0.3f) % 40;
          for (; x < SCR_W; x += w - 1) draw_sprite_screen(ids[k], x + d->ax * 0.25f, y, 0, 1, false, 32, 1 + k);
        }
      }
      break;
    case 3:
      grad = (const uint16_t *)(g_assets + ASSET_GRAD_LAB);
      draw_gradient(grad, 0);
      draw_tiled(SPR_lab_tile, (int)(px * 0.1f), (int)(py * 0.1f), 1);
      break;
    case 4000:
      draw_gradient((const uint16_t *)(g_assets + ASSET_GRAD_NIGHT), 0);
      break;
    case 4002:
      draw_gradient((const uint16_t *)(g_assets + ASSET_GRAD_SUNSET), 0);
      draw_sprite_screen(SPR_sunset_sun, 230, 70, 0, 1, false, 32, 1);
      {
        static const int ids[2] = { SPR_sunset_mountain1, SPR_sunset_mountain2 };
        for (int k = 0; k < 2; k++) {
          const SpriteDef *d = sprite_def(ids[k]);
          int w = d->w;
          int x = (int)(-px * (0.05f + 0.04f * k)) % w; if (x > 0) x -= w;
          for (; x < SCR_W; x += w - 1) draw_sprite_screen(ids[k], x + d->ax * 0.25f, 200 + 10 * k, 0, 1, false, 32, 2 + k);
        }
      }
      break;
    case 4003:
      draw_tiled(SPR_dots_tile, (int)(px * 0.1f), (int)(py * 0.1f), 1);
      break;
    case 2: case 4:
      draw_tiled(SPR_bricks_tile, (int)(px * 0.1f), (int)(py * 0.1f), 1);
      break;
    default:
      break;
  }
}
