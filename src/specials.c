// Level special items (types/params decoded from LevelB2D::addSpecial of the original game)
#include "game.h"
#include "gfx.h"
#include "level.h"
#include "../build/assets.h"
#include <string.h>

enum {
  SP_VAN = 0, SP_MINE = 2, SP_IBEAM = 3, SP_LOG = 4, SP_SPRING = 5, SP_SPIKES = 6, SP_WRECK = 7, SP_FAN = 8,
  SP_FINISH = 9, SP_SOCCER = 10, SP_BOOST = 12, SP_HARPOON = 15, SP_BOTTLE = 20, SP_SIGN = 23, SP_HOMING = 25,
  SP_JET = 28, SP_ARROWGUN = 29, SP_BLADE = 34
};

#define PX (1.0f / 62.5f)        // level pixels -> meters
#define SPX (1.0f / 41.6667f)    // sprite pixels -> meters
#define DEG (3.14159265f / 180.0f)

static VI s_sv[MAX_SPECIALS * 4 + 64];
static int s_nsv;

static const VI *sbox(float hw, float hh) {
  if (s_nsv + 4 > (int)(sizeof(s_sv) / sizeof(s_sv[0]))) s_nsv = 0;
  VI *v = &s_sv[s_nsv]; s_nsv += 4;
  int16_t x = (int16_t)lrintf(hw * 64), y = (int16_t)lrintf(hh * 64);
  v[0].x = -x; v[0].y = -y; v[1].x = x; v[1].y = -y; v[2].x = x; v[2].y = y; v[3].x = -x; v[3].y = y;
  return v;
}

// group-local -> world for specials living in groups (computed at creation)
static void spec_world(const LvSpecial *s, float *x, float *y, float *a) {
  *x = s->x; *y = s->y;
  if (s->group != 0xFFFF && s->group < L.h->nGroups) {
    const LvGroup *g = &L.groups[s->group];
    float c = cosf(g->a), sn = sinf(g->a);
    *x = g->x + c * s->x - sn * s->y;
    *y = g->y + sn * s->x + c * s->y;
    *a += g->a;
  }
}

int special_body(int idx) {
  for (int i = 0; i < L.nsp; i++) if (L.sp[i].rec == idx) return L.sp[i].body < 0 ? 0 : L.sp[i].body;
  return 0;
}

static int add_box(int b, float hw, float hh, float ox, float oy, float a, float dens, bool fixed, int coll, int spi) {
  int f = fix_poly(b, sbox(hw, hh), 4, ox, oy, a, dens);
  if (f < 0) return -1;
  uint16_t cat, mask; int16_t grp;
  static_filter(coll, fixed, &cat, &mask, &grp);
  fix_filter(f, cat, mask, grp);
  W.fix[f].tag = TAG_SPECIAL; W.fix[f].user = spi;
  return f;
}

// fixture inside a group body: offsets relative to group origin
static int group_body(const LvSpecial *s, float *lx, float *ly) {
  int b = L.groupBody[s->group];
  *lx = s->x; *ly = s->y;
  return b;
}

void special_create(int idx) {
  if (L.nsp >= MAX_SPECIALS) return;
  const LvSpecial *s = &L.specials[idx];
  Special *sp = &L.sp[L.nsp];
  memset(sp, 0, sizeof(*sp));
  sp->type = s->t; sp->rec = idx; sp->body = -1; sp->body2 = -1; sp->body3 = -1; sp->joint = -1;
  sp->z = s->z + Z_LEVEL;
  int spi = L.nsp;
  float x, y, a = 0;
  const float *p = s->p;
  bool inGroup = s->group != 0xFFFF && s->group < L.h->nGroups && L.groupBody[s->group] >= 0;
  switch (s->t) {
    case SP_IBEAM: case SP_LOG: {
      float w = p[0] * PX, h = p[1] * PX; a = p[2] * DEG;
      bool fixed = p[3] != 0;
      if (s->t == SP_LOG) fixed = p[3] != 0;
      if (inGroup) {
        // the beam is a fixture of the group body (drawn with it)
        float lx, ly; int b = group_body(s, &lx, &ly);
        // group bodies are already finalized: append is only possible for the last created body -> make it a welded body instead
        spec_world(s, &x, &y, &a);
        int nb = body_create(W.bodies[b].type == BT_STATIC ? BT_STATIC : BT_DYNAMIC, x, y, a);
        if (nb < 0) return;
        add_box(nb, w / 2, h / 2, 0, 0, 0, s->t == SP_LOG ? 0.8f : 2.0f, W.bodies[b].type == BT_STATIC, 1, spi);
        body_finalize(nb, 0);
        if (W.bodies[b].type != BT_STATIC) joint_weld(b, nb, x, y);
        if (!(W.bodies[b].flags & BF_AWAKE)) W.bodies[nb].flags &= ~BF_AWAKE;
        sp->body = nb;
      } else {
        x = s->x; y = s->y;
        int b = body_create(fixed ? BT_STATIC : BT_DYNAMIC, x, y, a);
        if (b < 0) return;
        add_box(b, w / 2, h / 2, 0, 0, 0, s->t == SP_LOG ? 0.8f : 2.0f, fixed, 1, spi);
        body_finalize(b, 0);
        if (p[4] != 0) W.bodies[b].flags &= ~BF_AWAKE;
        sp->body = b;
      }
      W.bodies[sp->body].tag = TAG_SPECIAL; W.bodies[sp->body].user = spi;
      sp->v0 = w; sp->v1 = h;
      break;
    }
    case SP_SPIKES: case SP_BLADE: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      bool fixed;
      float hw, hh;
      if (s->t == SP_SPIKES) { int n = (int)p[2]; if (n < 1) n = 1; sp->aux = n; hw = n * 0.16f * 0.5f + 0.04f; hh = 0.5f; fixed = p[1] != 0; }
      else { sp->aux = (int)p[4]; if (sp->aux < 1 || sp->aux > 12) sp->aux = 1; hw = 0.18f; hh = 0.65f; fixed = !(p[3] == 0 && p[2] == 0) ? (p[1] != 0 || p[3] != 0) : false; }
      int gb = inGroup ? L.groupBody[s->group] : -1;
      bool st = fixed || (gb >= 0 && W.bodies[gb].type == BT_STATIC);
      int b = body_create(st ? BT_STATIC : BT_DYNAMIC, x, y, a);
      if (b < 0) return;
      int f = add_box(b, hw, hh, 0, 0, 0, s->t == SP_BLADE ? 1.0f : 1.5f, st, 1, spi);
      (void)f;
      body_finalize(b, 0);
      if (gb >= 0 && !st) joint_weld(gb, b, x, y);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b;
      break;
    }
    case SP_MINE: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_STATIC, x, y, a);
      if (b < 0) return;
      int f = add_box(b, 0.2f, 0.05f, 0, -0.03f, 0, 1, true, 1, spi);
      if (f >= 0) W.fix[f].flags |= FXF_SENSOR;
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b;
      break;
    }
    case SP_SPRING: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_STATIC, x, y, a);
      if (b < 0) return;
      add_box(b, 233 * SPX * 0.5f, 0.25f, 0, 0, 0, 1, true, 1, spi);
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->v0 = p[1];
      break;
    }
    case SP_WRECK: {
      x = s->x; y = s->y;
      float len = p[0] * PX;
      if (len < 0.5f) len = 0.5f;
      int b = body_create(BT_DYNAMIC, x, y + len, 0);
      if (b < 0) return;
      int f = fix_circle(b, 0, 0, 0.62f, 4.0f);
      fix_filter(f, 8, 0xFFFF, 0);
      if (f >= 0) { W.fix[f].tag = TAG_SPECIAL; W.fix[f].user = spi; }
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->v0 = x; sp->v1 = y;
      sp->joint = joint_rope(0, b, x, y, x, y + len - 0.62f, len - 0.62f);
      // give it a small swing so it is alive like in the game
      W.bodies[b].vx = 2.0f;
      break;
    }
    case SP_FAN: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_STATIC, x, y, a);
      if (b < 0) return;
      add_box(b, 100 * SPX, 50 * SPX * 0.5f, 0, 25 * SPX, 0, 1, true, 1, spi);
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->v0 = a;
      sp->state = p[1] != 0 ? 1 : 1;
      break;
    }
    case SP_FINISH: {
      x = s->x; y = s->y;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_STATIC, x, y, 0);
      if (b < 0) return;
      int f = add_box(b, 1.2f, 2.0f, 0, -2.0f, 0, 1, true, 1, spi);
      if (f >= 0) { W.fix[f].flags |= FXF_SENSOR; fix_filter(f, 0x0F, 0x0100, 0); }
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b;
      break;
    }
    case SP_SOCCER: {
      x = s->x; y = s->y;
      int b = body_create(BT_DYNAMIC, x, y, 0);
      if (b < 0) return;
      int f = fix_circle(b, 0, 0, 6.5f * SPX, 0.5f);
      fix_filter(f, 8, 0xFFFF, 0);
      if (f >= 0) { W.fix[f].restitution = 70; W.fix[f].tag = TAG_SPECIAL; W.fix[f].user = spi; }
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b;
      break;
    }
    case SP_BOOST: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_STATIC, x, y, a);
      if (b < 0) return;
      int f = add_box(b, 180 * PX * 0.5f, 180 * PX * 0.5f, 0, 0, 0, 1, true, 1, spi);
      if (f >= 0) { W.fix[f].flags |= FXF_SENSOR; fix_filter(f, 0xFFFF, 0xFFFF, 0); }
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->v0 = p[2] > 0 ? p[2] : 20; sp->v1 = a;
      sp->state = p[1] != 0 ? 0 : 1;   // p3: starts disabled (trigger activated)
      break;
    }
    case SP_BOTTLE: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_DYNAMIC, x, y, a);
      if (b < 0) return;
      add_box(b, 3.5f * SPX, 9.5f * SPX, 0, 0, 0, 0.6f, false, 1, spi);
      body_finalize(b, 0);
      if (p[3] != 0 || p[2] != 0) W.bodies[b].flags &= ~BF_AWAKE;
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->aux = (int)p[1]; if (sp->aux < 1 || sp->aux > 4) sp->aux = 1;
      break;
    }
    case SP_HOMING: {
      x = s->x; y = s->y;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_DYNAMIC, x, y, 0);
      if (b < 0) return;
      int f = fix_circle(b, 0, 0, 7.5f * SPX, 1.0f);
      fix_filter(f, 8, 0xFFFF, 0);
      if (f >= 0) { W.fix[f].tag = TAG_SPECIAL; W.fix[f].user = spi; }
      body_finalize(b, 0);
      W.bodies[b].flags |= BF_NOGRAV;
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->v0 = p[0] > 0 ? p[0] : 3; sp->state = 0;
      break;
    }
    case SP_HARPOON: case SP_ARROWGUN: {
      a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_STATIC, x, y, a);
      if (b < 0) return;
      add_box(b, 0.35f, 0.35f, 0, 0, 0, 1, true, 1, spi);
      body_finalize(b, 0);
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->v0 = a; sp->t = 1.0f;
      sp->state = 1;
      break;
    }
    case SP_VAN: {
      a = p[0] * DEG;
      x = s->x; y = s->y;
      spec_world(s, &x, &y, &a);
      int b = body_create(BT_DYNAMIC, x, y, a);
      if (b < 0) return;
      add_box(b, 26 * SPX, 38 * SPX, 0, 0, 0, 1.2f, false, 1, spi);
      body_finalize(b, 0);
      if (p[2] != 0) W.bodies[b].flags &= ~BF_AWAKE;
      W.bodies[b].tag = TAG_SPECIAL; W.bodies[b].user = spi;
      sp->body = b; sp->flags = p[1] != 0;
      break;
    }
    case SP_SIGN: case SP_JET: default:
      x = s->x; y = s->y; a = p[0] * DEG;
      spec_world(s, &x, &y, &a);
      sp->v0 = x; sp->v1 = y; sp->t = a; sp->aux = (int)p[1];
      if (s->t == SP_SIGN && inGroup) sp->body3 = L.groupBody[s->group];
      break;
  }
  L.nsp++;
}

// ------------------------------------------------------------------ explosions
static void explode(float x, float y, float radius, float power) {
  for (int b = 1; b < W.nb; b++) {
    Body *bd = &W.bodies[b];
    if (bd->type != BT_DYNAMIC || (bd->flags & (BF_DEAD | BF_FROZEN))) continue;
    float dx = bd->cx - x, dy = bd->cy - y;
    float d2 = dx * dx + dy * dy;
    if (d2 > radius * radius) continue;
    float d = sqrtf(d2) + 0.05f;
    float k = power * (1 - d / radius) / d;
    float m = bd->mass < 0.3f ? bd->mass : 0.3f + (bd->mass - 0.3f) * 0.3f;
    body_apply_impulse(b, dx * k * m, (dy - 0.3f * d) * k * m, bd->cx, bd->cy);
  }
  particle_spawn(-2, x, y, 0, 0, 0.66f, 0);   // explosion animation
  for (int i = 0; i < 8; i++) particle_spawn(SPR_spark_particle_0 + (i % 3), x, y, frand() * 10 - 5, -frand() * 8, 0.6f, frand() * 8);
}

// ------------------------------------------------------------------ contacts
static Special *spec_of_fix(int f) {
  if (f & 0x8000) return 0;
  const Fixture *fx = &W.fix[f];
  if (fx->tag != TAG_SPECIAL) return 0;
  if (fx->user >= L.nsp) return 0;
  return &L.sp[fx->user];
}

static int other_body(int f) { return (f & 0x8000) ? 0 : W.fix[f].body; }

static void handle_contact(Special *sp, int otherFix, const Contact *c, int side) {
  int ob = other_body(otherFix);
  Body *o = ob > 0 ? &W.bodies[ob] : 0;
  bool isChar = ob > 0 && player_is_char_body(ob);
  bool isPlayer = ob > 0 && player_owns_body(ob);
  switch (sp->type) {
    case SP_MINE:
      if (sp->state == 0 && o && o->type == BT_DYNAMIC) {
        sp->state = 1;
        float x = W.bodies[sp->body].cx, y = W.bodies[sp->body].cy;
        body_kill(sp->body);
        explode(x, y - 0.2f, 4.0f, 22.0f);
        sp->v0 = x; sp->v1 = y; sp->t = 0;
      }
      break;
    case SP_SPIKES: {
      if (!isChar || !o) break;
      // impale when hitting the spiky side hard enough
      float vx = o->vx - W.bodies[sp->body].vx, vy = o->vy - W.bodies[sp->body].vy;
      float sp2 = vx * vx + vy * vy;
      if (sp2 > 9.0f && c->n > 0) {
        float wx = c->p[0].rax + W.bodies[W.fix[c->fa].body].cx, wy = c->p[0].ray + W.bodies[W.fix[c->fa].body].cy;
        player_impale(ob, sp->body, wx, wy);
        sp->flags |= 1;   // bloody
      }
      break;
    }
    case SP_BLADE: {
      if (!isChar || !o) break;
      float vx = o->vx - W.bodies[sp->body].vx, vy = o->vy - W.bodies[sp->body].vy;
      if (vx * vx + vy * vy > 16.0f) {
        player_impale(ob, sp->body, o->cx, o->cy);
        sp->flags |= 1;
      }
      break;
    }
    case SP_FINISH:
      if (isChar) game_level_complete();
      break;
    case SP_BOOST:
      if (sp->state && o && o->type == BT_DYNAMIC) {
        float a = W.bodies[sp->body].a;
        float dx = cosf(a), dy = sinf(a);
        float target = sp->v0 * 0.45f;
        float vAlong = o->vx * dx + o->vy * dy;
        if (vAlong < target) {
          float dv = (target - vAlong) * 0.35f;
          o->vx += dx * dv; o->vy += dy * dv;
          body_wake(ob);
        }
      }
      break;
    case SP_SPRING:
      if (o && o->type == BT_DYNAMIC && sp->state == 0) {
        sp->state = 1; sp->t = sp->v0;   // delay before firing
      }
      break;
    case SP_HOMING:
      if (sp->state == 1 && (isPlayer || (o && o->type == BT_DYNAMIC) || ob == 0)) {
        sp->state = 2;
        float x = W.bodies[sp->body].cx, y = W.bodies[sp->body].cy;
        body_kill(sp->body);
        explode(x, y, 3.5f, 18.0f);
      }
      break;
    case SP_BOTTLE:
      if (c->impulse > 0.25f && sp->state == 0) {
        sp->state = 1;
        float x = W.bodies[sp->body].cx, y = W.bodies[sp->body].cy;
        body_kill(sp->body);
        for (int i = 0; i < 6; i++) particle_spawn(SPR_van_glass_shard_2 + (i & 1), x, y, frand() * 4 - 2, -frand() * 3, 1.0f, frand() * 10 - 5);
      }
      break;
    case SP_VAN:
      if (c->impulse > 4.0f && !(sp->flags & 2)) {
        sp->flags |= 2;
        float x = W.bodies[sp->body].cx, y = W.bodies[sp->body].cy;
        for (int i = 0; i < 4; i++) particle_spawn(SPR_van_glass_shard_0 + i, x, y - 0.5f, frand() * 6 - 3, -frand() * 5, 1.2f, frand() * 6 - 3);
      }
      break;
    default: break;
  }
  (void)side; (void)isPlayer;
}

static void process_contacts(void) {
  for (int i = 0; i < W.nc; i++) {
    const Contact *c = &W.contacts[i];
    Special *a = spec_of_fix(c->fa), *b = spec_of_fix(c->fb);
    if (a) handle_contact(a, c->fb, c, 0);
    if (b) handle_contact(b, c->fa, c, 1);
  }
}

// ------------------------------------------------------------------ update
static int s_proj[12];   // projectile bodies (harpoons / arrows)
static int s_nproj;

static void fire_projectile(float x, float y, float a, bool harpoon) {
  int b = body_create(BT_DYNAMIC, x, y, a);
  if (b < 0) return;
  int f = fix_poly(b, sbox(harpoon ? 0.8f : 0.4f, 0.05f), 4, 0, 0, 0, 2.0f);
  fix_filter(f, 8, 0xFFFF, 0);
  if (f >= 0) { W.fix[f].tag = TAG_DEBRIS; W.fix[f].user = harpoon; }
  body_finalize(b, 0);
  float spd = harpoon ? 28 : 22;
  W.bodies[b].vx = cosf(a) * spd; W.bodies[b].vy = sinf(a) * spd;
  W.bodies[b].tag = TAG_DEBRIS; W.bodies[b].user = harpoon ? 1 : 2;
  W.bodies[b].flags |= BF_BULLET;
  if (s_nproj < 12) s_proj[s_nproj++] = b;
  else { body_kill(s_proj[0]); memmove(s_proj, s_proj + 1, sizeof(int) * 11); s_proj[11] = b; }
}

static void projectiles_update(void) {
  for (int i = 0; i < W.nc; i++) {
    const Contact *c = &W.contacts[i];
    for (int s = 0; s < 2; s++) {
      int f = s ? c->fb : c->fa, of = s ? c->fa : c->fb;
      if (f & 0x8000) continue;
      int b = W.fix[f].body;
      if (W.bodies[b].tag != TAG_DEBRIS || W.bodies[b].user == 0) continue;
      int ob = (of & 0x8000) ? 0 : W.fix[of].body;
      Body *pb = &W.bodies[b];
      float v2 = pb->vx * pb->vx + pb->vy * pb->vy;
      if (v2 < 25) { pb->user = 0; continue; }   // stuck, harmless
      if (ob > 0 && player_is_char_body(ob)) player_impale(ob, b, pb->cx, pb->cy);
      else if (ob == 0 || W.bodies[ob].type == BT_STATIC) { pb->vx = pb->vy = pb->w = 0; body_set_static(b, true); }
      pb->user = 0;
    }
  }
}

void special_activate(int idx, int action, const float *p) {
  for (int i = 0; i < L.nsp; i++) {
    Special *sp = &L.sp[i];
    if (sp->rec != idx) continue;
    switch (sp->type) {
      case SP_BOOST: sp->state = action == 1 ? 0 : 1; break;
      case SP_HOMING: if (sp->state == 0) sp->state = 1; break;
      case SP_HARPOON: case SP_ARROWGUN: sp->t = 0; sp->state = 2; break;
      case SP_FAN: sp->state = action == 1 ? 0 : 1; break;
      case SP_SPRING: if (sp->state == 0) { sp->state = 1; sp->t = 0; } break;
      default:
        if (sp->body > 0) {
          body_wake(sp->body);
          if (p && action == 1) body_apply_impulse(sp->body, p[0], p[1], W.bodies[sp->body].cx, W.bodies[sp->body].cy);
        }
        break;
    }
  }
}

void specials_update(float dt) {
  process_contacts();
  projectiles_update();
  float px = PL.x, py = PL.y;
  for (int i = 0; i < L.nsp; i++) {
    Special *sp = &L.sp[i];
    Body *b = sp->body > 0 ? &W.bodies[sp->body] : 0;
    switch (sp->type) {
      case SP_MINE: if (sp->state == 1) sp->t += dt; break;
      case SP_FAN: {
        if (!b || !sp->state) break;
        float a = b->a;
        float dx = sinf(a), dy = -cosf(a);       // blows "up" relative to the fan base
        float ox = b->cx, oy = b->cy;
        const float L1 = 9.0f, HW = 2.2f;
        for (int k = 1; k < W.nb; k++) {
          Body *o = &W.bodies[k];
          if (o->type != BT_DYNAMIC || (o->flags & (BF_DEAD | BF_FROZEN))) continue;
          float rx = o->cx - ox, ry = o->cy - oy;
          float along = rx * dx + ry * dy, side = rx * dy - ry * dx;
          if (along < 0 || along > L1 || side > HW || side < -HW) continue;
          float acc = 22.0f * (1 - along / L1);
          o->vx += dx * acc * dt; o->vy += dy * acc * dt;
          body_wake(k);
        }
        sp->t += dt;
        break;
      }
      case SP_SPRING:
        if (sp->state == 1) {
          sp->t -= dt;
          if (sp->t <= 0) {
            // launch everything resting on the top
            float a = b ? b->a : 0;
            float ux = sinf(a), uy = -cosf(a);
            for (int k = 0; k < W.nc; k++) {
              const Contact *c = &W.contacts[k];
              int o = -1;
              if (!(c->fb & 0x8000) && W.fix[c->fb].body == sp->body) o = W.fix[c->fa].body;
              if (W.fix[c->fa].body == sp->body && !(c->fb & 0x8000)) o = W.fix[c->fb].body;
              if (o > 0 && W.bodies[o].type == BT_DYNAMIC) {
                Body *ob = &W.bodies[o];
                float va = ob->vx * ux + ob->vy * uy;
                float dv = 14.0f - va;
                if (dv > 0) { ob->vx += ux * dv; ob->vy += uy * dv; }
                body_wake(o);
                // the rest of the player gets the push too
                if (player_owns_body(o))
                  for (int q = 1; q < W.nb; q++) if (player_owns_body(q)) { Body *pb = &W.bodies[q]; float v = pb->vx * ux + pb->vy * uy; if (v < 10) { pb->vx += ux * (12 - v); pb->vy += uy * (12 - v); } }
              }
            }
            sp->state = 2; sp->t = 0.6f;
          }
        } else if (sp->state == 2) { sp->t -= dt; if (sp->t <= 0) sp->state = 0; }
        break;
      case SP_HOMING:
        if (!b || (b->flags & BF_DEAD)) break;
        if (sp->state == 0) {
          float dx = px - b->cx, dy = py - b->cy;
          if (dx * dx + dy * dy < 8 * 8) sp->state = 1;
          b->vx *= 0.9f; b->vy *= 0.9f;
        }
        if (sp->state == 1) {
          float dx = px - b->cx, dy = py - b->cy;
          float d = sqrtf(dx * dx + dy * dy) + 0.01f;
          float spd = 3.0f + sp->v0 * 0.5f;
          b->vx += (dx / d * spd - b->vx) * 0.05f; b->vy += (dy / d * spd - b->vy) * 0.05f;
          body_wake(sp->body);
          sp->t += dt;
        }
        break;
      case SP_HARPOON: case SP_ARROWGUN: {
        if (!b) break;
        float dx = px - b->cx, dy = py - b->cy;
        float d2 = dx * dx + dy * dy;
        float aim = atan2f(dy, dx);
        if (d2 < 14 * 14 && !PL.dead) {
          float da = aim - sp->v0;
          while (da > 3.14159f) da -= 6.2832f;
          while (da < -3.14159f) da += 6.2832f;
          sp->v0 += clampf(da, -2 * dt, 2 * dt);
          sp->t -= dt;
          if (sp->t <= 0 && fabsf(da) < 0.15f) {
            fire_projectile(b->cx + cosf(sp->v0) * 0.8f, b->cy + sinf(sp->v0) * 0.8f, sp->v0, sp->type == SP_HARPOON);
            sp->t = sp->type == SP_HARPOON ? 3.0f : 1.6f;
          }
        }
        break;
      }
      case SP_BLADE: sp->t += dt; break;
      case SP_BOOST: sp->t += dt; break;
      default: break;
    }
  }
}

// ------------------------------------------------------------------ drawing
static void sprite_on(const Body *b, int spr, float lx, float ly, float rot, float scale, int z) {
  float ox, oy; body_origin(b, &ox, &oy);
  draw_sprite_world(spr, ox + b->c * lx - b->s * ly, oy + b->s * lx + b->c * ly, b->a + rot, scale, false, 32, z);
}

void specials_draw(void) {
  float wx0 = cam.x - SCR_W / 2 / cam.zoom - 6, wx1 = cam.x + SCR_W / 2 / cam.zoom + 6;
  float wy0 = cam.y - SCR_H / 2 / cam.zoom - 6, wy1 = cam.y + SCR_H / 2 / cam.zoom + 6;
  uint32_t fr = plat_frame_counter();
  for (int i = 0; i < L.nsp; i++) {
    Special *sp = &L.sp[i];
    const Body *b = sp->body > 0 ? &W.bodies[sp->body] : 0;
    float x = b ? b->cx : sp->v0, y = b ? b->cy : sp->v1;
    if (b && (b->flags & BF_DEAD) && sp->type != SP_MINE) continue;
    if (sp->type == SP_IBEAM || sp->type == SP_LOG) {
      if (!b) continue;
      float r = sqrtf(sp->v0 * sp->v0 + sp->v1 * sp->v1) * 0.5f;
      if (x + r < wx0 || x - r > wx1 || y + r < wy0 || y - r > wy1) continue;
    } else if (sp->type != SP_WRECK && (x < wx0 || x > wx1 || y < wy0 || y > wy1)) continue;
    int z = sp->z;
    switch (sp->type) {
      case SP_IBEAM: {
        float w = sp->v0, h = sp->v1;
        float sc = h / (21 * SPX);
        float tile = 133 * SPX * sc;
        int n = (int)(w / tile + 0.5f); if (n < 1) n = 1;
        float tw = w / n;
        float scx = tw / (133 * SPX);
        for (int k = 0; k < n; k++) sprite_on(b, SPR_ibeam, -w / 2 + tw * (k + 0.5f), 0, 0, scx > sc ? scx : sc, z);
        break;
      }
      case SP_LOG: {
        float w = sp->v0, h = sp->v1;
        float sc = w / (24 * SPX);
        float top = 139 * SPX * sc, bot = 141 * SPX * sc, cen = 70 * SPX * sc;
        float mid = h - top - bot;
        sprite_on(b, SPR_log_top, 0, -h / 2 + top / 2, 0, sc, z);
        float yy = -h / 2 + top;
        while (mid > 0.01f) { sprite_on(b, SPR_log_center, 0, yy + cen / 2, 0, sc, z); yy += cen; mid -= cen; }
        sprite_on(b, SPR_log_bottom, 0, h / 2 - bot / 2, 0, sc, z + 1);
        break;
      }
      case SP_SPIKES: {
        int n = sp->aux;
        for (int k = 0; k < n; k++) sprite_on(b, SPR_spike, -n * 0.08f + 0.08f + k * 0.16f, 0, 0, 1, z);
        if (sp->flags & 1) for (int k = 0; k < n && k < 5; k++) sprite_on(b, SPR_spike_blood_1_0 + k, -n * 0.08f + 0.08f + k * 0.32f, -0.2f, 0, 1, z + 1);
        break;
      }
      case SP_BLADE:
        sprite_on(b, SPR_blade_1 + (sp->aux - 1), 0, 0, 0, 1, z);
        break;
      case SP_MINE:
        if (sp->state == 0) {
          if (b) {
            sprite_on(b, SPR_mine, 0, 0, 0, 1, z);
            if ((fr / 15) & 1) sprite_on(b, SPR_mine_light, 0, -0.07f, 0, 1, z + 1);
          }
        } else if (sp->t < 0.66f) {
          int f = (int)(sp->t * 30); if (f > 19) f = 19;
          draw_sprite_world(SPR_mineExplosion_1 + f, sp->v0, sp->v1, 0, 1, false, 32, Z_PART);
        }
        break;
      case SP_SPRING: {
        float up = sp->state == 2 ? 0.5f : 0;
        sprite_on(b, SPR_springbox_base, 0, 0.1f, 0, 1, z);
        sprite_on(b, SPR_springbox_piston, -1.5f, -0.1f - up * 0.5f, 0, 1, z + 1);
        sprite_on(b, SPR_springbox_piston, 1.5f, -0.1f - up * 0.5f, 0, 1, z + 1);
        sprite_on(b, SPR_springbox_top, 0, -0.2f - up, 0, 1, z + 2);
        break;
      }
      case SP_WRECK: {
        if (!b) break;
        draw_line_world(sp->v0, sp->v1, b->cx, b->cy, 2, rgb(60, 60, 60), z);
        float ang = atan2f(b->cy - sp->v1, b->cx - sp->v0) - 1.5708f;
        draw_sprite_world(SPR_wreckingball, b->cx - sinf(ang) * 0.0f, b->cy, ang, 1, false, 32, z + 1);
        draw_sprite_world(SPR_wreckingball_anchor, sp->v0, sp->v1, 0, 1, false, 32, z + 2);
        break;
      }
      case SP_FAN:
        sprite_on(b, SPR_fan_base, 0, 0, 0, 1, z);
        sprite_on(b, SPR_fan_blade_1 + ((fr / 2) % 3), 0, -0.9f, 0, 1, z + 1);
        sprite_on(b, SPR_fan_cover, 0, -1.25f, 0, 1, z + 2);
        break;
      case SP_FINISH: {
        sprite_on(b, SPR_finishline_pole, -1.0f, -0.16f, 0, 1, z);
        sprite_on(b, SPR_finishline_pole, 1.0f, -0.16f, 0, 1, z);
        sprite_on(b, SPR_finishline_banner, -1.0f, -0.6f, 0, 1, z);
        sprite_on(b, SPR_finishline_banner, 1.0f, -0.6f, 0, 1, z);
        sprite_on(b, SPR_finishline_cap, -1.0f, -0.95f, 0, 1, z);
        sprite_on(b, SPR_finishline_cap, 1.0f, -0.95f, 0, 1, z);
        sprite_on(b, SPR_flag_frame_1 + (fr / 3) % 18, 0, -3.2f, 0, 1, z + 1);
        break;
      }
      case SP_SOCCER: sprite_on(b, SPR_soccerball, 0, 0, 0, 1, z); break;
      case SP_BOOST: {
        int f = sp->state ? (int)(sp->t * 12) % 4 : 0;
        sprite_on(b, SPR_boostpanel_1 + f, 0, 0, 0, 1, z);
        break;
      }
      case SP_BOTTLE: sprite_on(b, SPR_bottle_1 + sp->aux - 1, 0, 0, 0, 1, z); break;
      case SP_HOMING:
        if (!b || (b->flags & BF_DEAD)) break;
        sprite_on(b, SPR_homingMine, 0, 0, 0, 1, z);
        sprite_on(b, sp->state == 0 ? SPR_homingMine_green : ((fr / 4) & 1 ? SPR_homingMine_red : SPR_homingMine_yellow), 0, -0.05f, 0, 1, z + 1);
        if (sp->state == 1) sprite_on(b, SPR_homingMine_jet, 0, 0.22f, 0, 1, z - 1);
        break;
      case SP_HARPOON:
        draw_sprite_world(SPR_harpoon_turret, x, y, sp->v0 + 1.5708f, 1, false, 32, z);
        draw_sprite_world(SPR_harpoon_base, x, y, 0, 1, false, 32, z + 1);
        break;
      case SP_ARROWGUN:
        draw_sprite_world(SPR_arrow_gun_turret, x, y, sp->v0 + 1.5708f, 1, false, 32, z);
        draw_sprite_world(SPR_arrow_gun_base, x, y, 0, 1, false, 32, z + 1);
        break;
      case SP_VAN:
        sprite_on(b, (sp->flags & 2) ? SPR_van_smashed : SPR_van, 0, 0, 0, (sp->flags & 2) ? 0.9f : 1, z);
        break;
      case SP_SIGN: {
        float a = sp->t;
        float c = cosf(a), s = sinf(a);
        float tx = x + s * 1.9f, ty = y - c * 1.9f;   // sign face on top of the post
        draw_sprite_world(SPR_sign_post, x + s * 1.14f, y - c * 1.14f, a, 1, false, 32, z);
        int t = sp->aux;
        if (t >= 7 && t <= 13) {
          draw_sprite_world(SPR_sign_back_yellow, tx, ty, a, 1, false, 32, z + 1);
          draw_sprite_world(SPR_sign_decal_7 + (t - 7), tx, ty + 0.05f, a, 1, false, 32, z + 2);
        } else if (t == 3) draw_sprite_world(SPR_sign_stop, tx, ty, a, 1, false, 32, z + 1);
        else if (t == 4) draw_sprite_world(SPR_sign_slow, tx, ty, a, 1, false, 32, z + 1);
        else {
          bool v = (t == 2 || t == 6);
          draw_sprite_world(v ? SPR_sign_back_green_v : SPR_sign_back_green_h, tx, ty, a, 1, false, 32, z + 1);
          float ar = (t == 5) ? 3.14159f : (t == 2 ? -1.5708f : (t == 6 ? 1.5708f : 0));
          draw_sprite_world(SPR_sign_arrow, tx, ty, a + ar, 1, false, 32, z + 2);
        }
        break;
      }
      case SP_JET:
        draw_sprite_world(SPR_jet_body, x, y, sp->t, 1, false, 32, z);
        break;
      default: break;
    }
  }
  // projectiles
  for (int i = 0; i < s_nproj; i++) {
    const Body *b = &W.bodies[s_proj[i]];
    if (b->flags & BF_DEAD) continue;
    const Fixture *f = &W.fix[b->firstFix];
    draw_sprite_world(f->user ? SPR_harpoon : SPR_arrow_1, b->cx, b->cy, b->a, 1, false, 32, Z_CHAR - 10);
  }
}

void specials_reset(void) { s_nproj = 0; s_nsv = 0; }
