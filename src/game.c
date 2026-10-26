// Game flow: menus, level loop, triggers, camera, particles, HUD.
#include "game.h"
#include "gfx.h"
#include "level.h"
#include "platform.h"
#include "../build/assets.h"
#include <string.h>
#ifndef PLATFORM_DEVICE
#include <stdio.h>
#include <stdlib.h>
#endif

Input IN;
static uint32_t s_frame;
uint32_t plat_frame_counter(void) { return s_frame; }

static uint32_t s_rng = 12345;
float frand(void) { s_rng = s_rng * 1664525u + 1013904223u; return (s_rng >> 8) * (1.0f / 16777216.0f); }

void specials_reset(void);

// ------------------------------------------------------------------ arena
static uint8_t *s_arena; static int s_arenaSize, s_arenaUsed;
static void *arena_alloc(int n) {
  n = (n + 7) & ~7;
  if (s_arenaUsed + n > s_arenaSize) return 0;
  void *p = s_arena + s_arenaUsed; s_arenaUsed += n;
  memset(p, 0, n);
  return p;
}
int game_arena_used(void) { return s_arenaUsed; }

// ------------------------------------------------------------------ particles
#define MAXP 80
typedef struct { float x, y, vx, vy, a, w, life, t; int16_t spr; } Particle;
static Particle s_p[MAXP];
static int s_np;

void particles_reset(void) { s_np = 0; }
void particle_spawn(int spr, float x, float y, float vx, float vy, float life, float spin) {
  if (spr < 0) return;
  Particle *p;
  if (s_np < MAXP) p = &s_p[s_np++];
  else p = &s_p[plat_random() % MAXP];
  p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->a = frand() * 6.28f; p->w = spin; p->life = life; p->t = 0; p->spr = spr;
}
void particle_blood(float x, float y, float vx, float vy, int n) {
  for (int i = 0; i < n; i++)
    particle_spawn(SPR_blood_particle_0 + (i & 3), x, y, vx + frand() * 5 - 2.5f, vy + frand() * 4 - 3, 0.8f + frand() * 0.8f, 0);
}
void particles_update(float dt) {
  for (int i = 0; i < s_np; i++) {
    Particle *p = &s_p[i];
    p->t += dt;
    if (p->t >= p->life) { s_p[i] = s_p[--s_np]; i--; continue; }
    p->vy += 10 * dt;
    p->x += p->vx * dt; p->y += p->vy * dt; p->a += p->w * dt;
  }
}
void particles_draw(void) {
  for (int i = 0; i < s_np; i++) {
    Particle *p = &s_p[i];
    float k = 1 - p->t / p->life;
    uint8_t al = (uint8_t)(k > 0.5f ? 32 : 64 * k);
    draw_sprite_world(p->spr, p->x, p->y, p->a, 1, false, al, Z_PART + i);
  }
}

// ------------------------------------------------------------------ triggers
typedef struct { int16_t shape, body; float from, to, t, dur; } Fade;
static Fade s_fades[24];
static int s_nfades;
static float s_timeScale = 1;

static int shape_body(int top) {
  if (top < 0 || top >= L.h->nTop) return -1;
  uint16_t r = L.remap[top];
  if (r == 0xFFFF) return -1;
  return L.shapeBody[r];
}

static void set_body_collision(int b, int coll) {
  Body *bd = &W.bodies[b];
  for (int k = 0; k < bd->nFix; k++) {
    Fixture *f = &W.fix[bd->firstFix + k];
    uint16_t cat, mask; int16_t g;
    static_filter(coll, bd->type == BT_STATIC, &cat, &mask, &g);
    f->cat = cat; f->mask = mask; f->group = g;
    if (coll == 3) f->flags |= FXF_SENSOR; else f->flags &= ~FXF_SENSOR;
  }
  body_wake(b);
}

static void add_fade(int shape, int body, float from, float to, float dur) {
  if (s_nfades >= 24) return;
  Fade *f = &s_fades[s_nfades++];
  f->shape = shape; f->body = body; f->from = from; f->to = to; f->t = 0; f->dur = dur > 0.01f ? dur : 0.01f;
}

static void body_action(int b, int a, const float *p, bool group, int shapeIdx) {
  if (b <= 0) return;
  Body *bd = &W.bodies[b];
  int act = a;
  if (group) {
    // group action order: awake, opacity, impulse, fix, non-fix, delete shapes, delete self, collision
    static const int8_t map[8] = { 0, 3, 4, 1, 2, 5, 6, 7 };
    act = a < 8 ? map[a] : -1;
  }
  switch (act) {
    case 0: body_wake(b); break;
    case 1: body_set_static(b, true); break;
    case 2: body_set_static(b, false); body_wake(b); break;
    case 3:
      if (group) add_fade(-1, b, L.bdraw[b].opac, p[0], p[1]);
      else if (shapeIdx >= 0) add_fade(shapeIdx, -1, L.shapeAlpha[shapeIdx], p[0], p[1]);
      break;
    case 4:
      if (bd->type == BT_DYNAMIC) {
        body_wake(b);
        bd->vx += p[0] * 0.2f; bd->vy += p[1] * 0.2f; bd->w += p[2] * 0.1f;
      }
      break;
    case 5: case 6: body_kill(b); break;
    case 7: set_body_collision(b, (int)p[0]); break;
  }
}

static void fire_trigger(int ti);

static void run_targets(int ti) {
  const LvTrigger *t = &L.triggers[ti];
#ifndef PLATFORM_DEVICE
  if (getenv("HW_TRIG")) printf("frame %u trigger %d type %d by %d n %d at %.1f,%.1f\n", s_frame, ti, t->type, t->by, t->n, t->x, t->y);
#endif
  if (t->type == 3) { game_level_complete(); return; }
  if (t->type == 250) { s_timeScale = 0.4f; return; }
  if (t->type == 251) { s_timeScale = 1; return; }
  if (t->type != 1) return;
  for (int k = 0; k < t->n; k++) {
    const LvTarget *g = &L.targets[t->first + k];
    switch (g->k) {
      case 0: {
        int b = shape_body(g->i);
        int si = (g->i < L.h->nTop) ? L.remap[g->i] : 0xFFFF;
        body_action(b, g->a, g->p, false, si == 0xFFFF ? -1 : si);
        break;
      }
      case 1: if (g->i < L.h->nGroups) body_action(L.groupBody[g->i], g->a, g->p, true, -1); break;
      case 2: {
        if (g->i >= L.h->nJoints) break;
        int j = L.jointMap[g->i];
        if (j < 0) break;
        Joint *jt = &W.joints[j];
        bool rev = jt->type == JT_REVOLUTE;
        switch (g->a) {
          case 0: jt->flags &= ~JF_MOTOR; break;
          case 1: jt->flags |= JF_MOTOR; jt->speed = g->p[0]; if (jt->maxF <= 0) jt->maxF = rev ? 50 : 50; break;
          case 2: joint_kill(j); break;
          case 3: jt->flags &= ~JF_LIMIT; break;
          case 4:
            jt->flags |= JF_LIMIT;
            if (rev) { jt->hi = g->p[0] * 3.14159265f / 180; jt->lo = g->p[1] * 3.14159265f / 180; }
            else { jt->hi = g->p[0] / 62.5f; jt->lo = g->p[1] / 62.5f; }
            if (jt->lo > jt->hi) { float x = jt->lo; jt->lo = jt->hi; jt->hi = x; }
            break;
        }
        body_wake(jt->ba); body_wake(jt->bb);
        break;
      }
      case 3: special_activate(g->i, g->a, g->p); break;
      case 4:
        if (g->i >= L.h->nTriggers) break;
        if (g->a == 0) fire_trigger(g->i);
        else if (g->a == 1) L.trig[g->i].enabled = 0;
        else L.trig[g->i].enabled = 1;
        break;
    }
  }
}

static void fire_trigger(int ti) {
  TrigRT *r = &L.trig[ti];
  const LvTrigger *t = &L.triggers[ti];
  if (!r->enabled) return;
  if (t->d > 0) { if (r->delay <= 0) r->delay = t->d + 0.0001f; }
  else run_targets(ti);
}

typedef struct { const LvTrigger *t; float c, s; int hit; } TrigQ;

static bool point_in_trigger(const LvTrigger *t, float c, float s, float x, float y, float pad) {
  float dx = x - t->x, dy = y - t->y;
  float lx = c * dx + s * dy, ly = -s * dx + c * dy;
  return fabsf(lx) <= t->w / 2 + pad && fabsf(ly) <= t->h / 2 + pad;
}

static void triggers_update(float dt) {
  for (int i = 0; i < L.h->nTriggers; i++) {
    const LvTrigger *t = &L.triggers[i];
    TrigRT *r = &L.trig[i];
    if (r->delay > 0) {
      r->delay -= dt;
      if (r->delay <= 0) { r->delay = 0; run_targets(i); }
    }
    if (!r->enabled || t->by == 5 || t->type == 2 || t->type == 252) continue;
    if (t->rep == 1 && r->fired) continue;
    // quick distance cull: triggers far from the player only react to objects (rare)
    bool inside = false;
    float c = cosf(t->a), s = sinf(t->a);
    if (t->by == 1 || t->by == 2 || t->by == 3) {
      for (int b = L.bodyStart; b < W.nb && !inside; b++) {
        const Body *bd = &W.bodies[b];
        if (bd->flags & BF_DEAD) continue;
        if (bd->tag != TAG_CHAR && bd->tag != TAG_VEH) continue;
        if (point_in_trigger(t, c, s, bd->cx, bd->cy, 0.15f)) inside = true;
      }
    }
    if (!inside && (t->by == 3 || t->by == 4)) {
      float r2 = (t->w + t->h) * 0.5f + 2;
      for (int b = 1; b < W.nb && !inside; b++) {
        const Body *bd = &W.bodies[b];
        if (bd->type != BT_DYNAMIC || (bd->flags & (BF_DEAD | BF_FROZEN))) continue;
        if (bd->tag == TAG_CHAR || bd->tag == TAG_VEH) continue;
        if (fabsf(bd->cx - t->x) > r2 || fabsf(bd->cy - t->y) > r2) continue;
        if (point_in_trigger(t, c, s, bd->cx, bd->cy, 0.2f)) inside = true;
      }
    }
    if (inside) {
      if (!r->inside) {
        r->inside = 1; r->fired = 1; r->timer = 0;
        fire_trigger(i);
      } else if (t->rep == 3) {
        r->timer += dt;
        float iv = t->i > 0.02f ? t->i : 0.5f;
        if (r->timer >= iv) { r->timer = 0; fire_trigger(i); }
      } else if (t->rep == 4) fire_trigger(i);
    } else {
      r->inside = 0;
    }
  }
  // opacity fades
  for (int i = 0; i < s_nfades; i++) {
    Fade *f = &s_fades[i];
    f->t += dt;
    float k = clampf(f->t / f->dur, 0, 1);
    float v = f->from + (f->to - f->from) * k;
    uint8_t o = (uint8_t)clampf(v, 0, 100);
    if (f->shape >= 0) L.shapeAlpha[f->shape] = o; else if (f->body > 0) L.bdraw[f->body].opac = o;
    if (k >= 1) { s_fades[i] = s_fades[--s_nfades]; i--; }
  }
}

// ------------------------------------------------------------------ states
enum { ST_TITLE, ST_CHARSEL, ST_LEVELSEL, ST_PLAY, ST_PAUSE, ST_COMPLETE, ST_ERROR };
static int s_state = ST_TITLE;
static int s_chapter, s_levelInChapter, s_menuSel, s_allChar = 2;
static int s_level = -1;
static bool s_complete;
static float s_completeTime;
static uint8_t s_done[NLEVELS];

static bool load_level(int lv) {
  s_arenaUsed = 0;
  particles_reset();
  specials_reset();
  s_nfades = 0; s_timeScale = 1;
  s_complete = false;
  if (!level_load(lv, arena_alloc)) return false;
  int chr = LEVELS[lv].chr > 0 ? LEVELS[lv].chr : s_allChar;
  if (!LEVELS[lv].forced && CHAPTERS[s_chapter].chr <= 0) chr = s_allChar;
  player_create(chr, L.h->sx, L.h->sy);
  if (W.nb >= W.maxb - 2 || W.nf >= W.maxf - 2) { /* overflow: still playable */ }
  cam.x = PL.x; cam.y = PL.y - 0.5f;
  s_level = lv;
  L.time = 0;
  return true;
}

void game_level_complete(void) {
  if (s_complete || PL.dead) return;
  s_complete = true;
  s_completeTime = L.time;
  s_done[s_level] = 1;
}

static void update_camera(float dt) {
  float tx = PL.x + clampf(PL.vx * 0.25f, -2.5f, 2.5f);
  float ty = PL.y - 0.6f + clampf(PL.vy * 0.15f, -1.5f, 1.5f);
  float k = clampf(dt * 5, 0, 1);
  cam.x += (tx - cam.x) * k;
  cam.y += (ty - cam.y) * k;
  // keep the player on screen whatever happens
  float hw = SCR_W / 2 / cam.zoom - 1, hh = SCR_H / 2 / cam.zoom - 1;
  cam.x = clampf(cam.x, PL.x - hw, PL.x + hw);
  cam.y = clampf(cam.y, PL.y - hh, PL.y + hh);
  W.activeX0 = cam.x - 22; W.activeX1 = cam.x + 22;
  W.activeY0 = cam.y - 16; W.activeY1 = cam.y + 16;
}

static void play_step(float dt) {
  player_update(dt);
  int sub = (int)(dt * 60 + 0.5f);
  if (sub < 2) sub = 2;
  if (sub > 3) sub = 3;
  float h = dt * s_timeScale / sub;
  for (int i = 0; i < sub; i++) {
    phys_step(h);
    player_after_step(h);
    specials_update(h);
  }
  // bodies that fell out of the world are removed (saves CPU)
  float killY = L.h->by1 + 40;
  for (int b = 1; b < W.nb; b++) {
    Body *bd = &W.bodies[b];
    if (bd->type != BT_DYNAMIC || (bd->flags & BF_DEAD) || bd->cy < killY) continue;
    if (player_owns_body(b)) { player_kill(); continue; }   // fell out of the world
    body_kill(b);
  }
  triggers_update(dt * s_timeScale);
  particles_update(dt * s_timeScale);
  if (!s_complete) L.time += dt;
#ifndef PLATFORM_DEVICE
  if (getenv("HW_DEBUG3") && s_frame > 40 && s_frame < 52) {
    for (int i = 0; i < W.nc; i++) {
      const Contact *c = &W.contacts[i];
      int ba = W.fix[c->fa].body;
      if (W.bodies[ba].tag != TAG_CHAR && W.bodies[ba].tag != TAG_VEH) continue;
      if (c->fb & 0x8000) { const StaticPrim *sp = &W.sw.prims[c->fb & 0x7FFF]; printf("f%u body%d(tag%d part%d) static kind%d n=%d imp=%.2f nrm %.2f,%.2f\n", s_frame, ba, W.bodies[ba].tag, W.bodies[ba].user, sp->kind, sp->n, c->impulse, c->nx, c->ny); }
      else { int bb = W.fix[c->fb].body; printf("f%u body%d(tag%d part%d) vs body%d tag%d user%d imp=%.2f\n", s_frame, ba, W.bodies[ba].tag, W.bodies[ba].user, bb, W.bodies[bb].tag, W.bodies[bb].user, c->impulse); }
    }
  }
  if (getenv("HW_DEBUG2") && s_frame % 10 == 0) {
    printf("f%u pl %.2f,%.2f v %.2f,%.2f", s_frame, PL.x, PL.y, PL.vx, PL.vy);
    for (int i = 0; i < 3; i++) if (PL.vb[i] >= 0) { Body *b = &W.bodies[PL.vb[i]]; printf(" | b%d %.2f,%.2f a%.2f w%.2f %s", i, b->cx, b->cy, b->a, b->w, (b->flags & BF_AWAKE) ? "aw" : "zz"); }
    for (int i = 0; i < 2; i++) if (PL.vj[i] >= 0) { Joint *j = &W.joints[PL.vj[i]]; printf(" | j%d fl%x sp%.1f mf%.1f im%.2f", i, j->flags, j->speed, j->maxF, j->im); }
    printf(" nc %d\n", W.nc);
  }
#endif
  update_camera(dt);
}

static char *put_int(char *p, int v, int digits) {
  char tmp[8]; int n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 8);
  while (n < digits) tmp[n++] = '0';
  while (n) *p++ = tmp[--n];
  return p;
}

static void draw_time(float t, int x, int y) {
  char buf[16];
  int m = (int)(t / 60), s = (int)t % 60, cs = (int)(t * 100) % 100;
  char *p = put_int(buf, m, 1); *p++ = ':'; p = put_int(p, s, 2); *p++ = '.'; p = put_int(p, cs, 2); *p = 0;
  draw_number(buf, x, y, Z_HUD + 1, 1);
}

static void draw_play_scene(void) {
  gfx_begin();
  level_draw_background();
  level_draw();
  specials_draw();
  player_draw();
  particles_draw();
  // HUD: timer like the original (top right)
  draw_sprite_screen(SPR_controls_timer_bg, 320 - 50, 14, 0, 1, false, 32, Z_HUD);
  draw_time(L.time, 320 - 82, 6);
  if (PL.dead && PL.deadTimer > 1.0f && (s_frame / 15) & 1)
    draw_sprite_screen(T_PRESS_EXE_TO_RESTART, 160, 200, 0, 1, false, 32, Z_HUD + 2);
}

static void center_sprite(int id, int y, int z) {
  const SpriteDef *d = sprite_def(id);
  draw_sprite_screen(id, 160 - d->w / 2 + d->ax * 0.25f, y, 0, 1, false, 32, z);
}

static void draw_menu_bg(void) {
  gfx_begin();
  draw_sprite_screen(SPR_menu_bg, 0, 0, 0, 1, false, 32, 0);
}

static void credits_footer(int z) {
  draw_rect(0, 206, 320, 34, 0, 18, z);
  center_sprite(T_DISCORD_GG_STI2D, 214, z + 1);
  center_sprite(T_DISCORD_CJNW_YAMII, 230, z + 1);
}

static bool s_quit;

static void title_screen(void) {
  if (in_pressed(K_BACK)) s_quit = true;
  draw_menu_bg();
  const SpriteDef *lg = sprite_def(SPR_menu_main_logo);
  draw_sprite_screen(SPR_menu_main_logo, 160 - lg->w * 0.35f + lg->ax * 0.25f * 0.7f, 18 + lg->ay * 0.25f * 0.7f, 0, 0.7f, false, 32, 2);
  draw_sprite_screen(SPR_menu_main_playbtn_blue_normal, 160, 145, 0, 0.8f, false, 32, 3);
  draw_sprite_screen(T_PLAY, 160, 145, 0, 1, false, 32, 4);
  center_sprite(T_NUMWORKS_PORT, 182, 4);
  center_sprite(T_ORIGINAL_GAME_FANCY_FORC, 196, 4);
  credits_footer(5);
  gfx_render(0);
  if (in_pressed(K_OK) || in_pressed(K_EXE)) { s_state = ST_CHARSEL; s_menuSel = s_chapter; }
}

static const int16_t ICONS[] = { SPR_biz_icon, SPR_dad_icon, SPR_whe_icon, SPR_sho_icon, SPR_pogo_icon, SPR_mop_icon };
static const int16_t ICONS_BW[] = { SPR_biz_icon_bw, SPR_dad_icon_bw, SPR_whe_icon_bw, SPR_sho_icon_bw, SPR_pogo_icon_bw, SPR_mop_icon_bw };

static void charsel_screen(void) {
  if (in_pressed(K_LEFT) && s_menuSel > 0) s_menuSel--;
  if (in_pressed(K_RIGHT) && s_menuSel < NCHAPTERS - 1) s_menuSel++;
  draw_menu_bg();
  center_sprite(T_SELECT_A_CHARACTER, 14, 2);
  int n = NCHAPTERS;
  int sp = 52;
  int x0 = 160 - (n - 1) * sp / 2;
  for (int i = 0; i < n; i++) {
    int x = x0 + i * sp, y = 95;
    bool sel = i == s_menuSel;
    draw_sprite_screen(sel ? SPR_charSel_frame_yellow : SPR_charSel_frame, x, y, 0, 0.9f, false, 32, 3);
    int ic = i < 5 ? i : 5;
    if (CHAPTERS[i].chr <= 0) ic = 5;
    draw_sprite_screen(sel ? ICONS[ic] : ICONS_BW[ic], x, y, 0, 0.9f, false, 32, 4);
  }
  center_sprite(CHAPTER_TXT[s_menuSel], 140, 4);
  int done = 0;
  for (int k = 0; k < CHAPTERS[s_menuSel].n; k++) done += s_done[CHAPTERS[s_menuSel].first + k];
  char buf[16]; char *p = put_int(buf, done, 1); *p++ = ':'; p = put_int(p, CHAPTERS[s_menuSel].n, 1); *p = 0;
  draw_number(buf, 160 - 18, 160, 5, 1);
  credits_footer(6);
  gfx_render(0);
  if (in_pressed(K_OK) || in_pressed(K_EXE)) { s_chapter = s_menuSel; s_state = ST_LEVELSEL; s_menuSel = 0; }
  if (in_pressed(K_BACK)) s_state = ST_TITLE;
}

static void levelsel_screen(void) {
  const ChapterEntry *ch = &CHAPTERS[s_chapter];
  int n = ch->n;
  if (in_pressed(K_LEFT) && s_menuSel > 0) s_menuSel--;
  if (in_pressed(K_RIGHT) && s_menuSel < n - 1) s_menuSel++;
  if (in_pressed(K_UP) && s_menuSel >= 5) s_menuSel -= 5;
  if (in_pressed(K_DOWN) && s_menuSel + 5 < n) s_menuSel += 5;
  bool allChapter = ch->chr <= 0;
  if (allChapter) {
    static const int chars[5] = { 2, 3, 1, 4, 9 };
    int ci = 0; for (int i = 0; i < 5; i++) if (chars[i] == s_allChar) ci = i;
    if (in_pressed(K_PLUS)) s_allChar = chars[(ci + 1) % 5];
    if (in_pressed(K_MINUS)) s_allChar = chars[(ci + 4) % 5];
  }
  draw_menu_bg();
  center_sprite(CHAPTER_TXT[s_chapter], 12, 2);
  for (int i = 0; i < n; i++) {
    int col = i % 5, row = i / 5;
    int x = 40 + col * 60, y = 58 + row * 50;
    bool sel = i == s_menuSel;
    // levSelect_N sprites are sorted alphabetically: 1,10..15,2..9
    static const uint8_t ord[15] = { 0, 7, 8, 9, 10, 11, 12, 13, 14, 1, 2, 3, 4, 5, 6 };
    draw_sprite_screen(SPR_levSelect_icon, x, y, 0, 0.62f, false, 32, 3);
    draw_sprite_screen(SPR_levSelect_1 + ord[i < 15 ? i : 14], x, y, 0, 1, false, 32, 4);
    if (s_done[ch->first + i]) draw_sprite_screen(SPR_levSelect_check, x + 14, y - 14, 0, 1, false, 32, 5);
    if (sel) draw_sprite_screen(SPR_charSel_frame_yellow, x, y, 0, 0.88f, false, 32, 6);
  }
  center_sprite(LEVEL_TXT[ch->first + s_menuSel], 190, 6);
  if (allChapter) {
    int ic = s_allChar == 2 ? 0 : s_allChar == 3 ? 1 : s_allChar == 1 ? 2 : s_allChar == 4 ? 3 : 4;
    draw_sprite_screen(ICONS[ic], 296, 190, 0, 0.5f, false, 32, 6);
  }
  credits_footer(7);
  gfx_render(0);
  if (in_pressed(K_OK) || in_pressed(K_EXE)) {
    s_levelInChapter = s_menuSel;
    if (load_level(ch->first + s_menuSel)) s_state = ST_PLAY; else s_state = ST_ERROR;
  }
  if (in_pressed(K_BACK)) { s_state = ST_CHARSEL; s_menuSel = s_chapter; }
}

static void pause_screen(void) {
  static const int16_t items[4] = { T_RESUME, T_RESTART, T_LEVEL_SELECT, T_MAIN_MENU };
  static const int16_t icons[4] = { SPR_menu_pause_icon_resume, SPR_menu_pause_icon_reset, SPR_menu_pause_icon_exit, SPR_menu_pause_icon_character };
  if (in_pressed(K_UP) && s_menuSel > 0) s_menuSel--;
  if (in_pressed(K_DOWN) && s_menuSel < 3) s_menuSel++;
  draw_play_scene();
  draw_rect(0, 0, 320, 240, 0, 16, Z_HUD + 10);
  center_sprite(T_PAUSED, 22, Z_HUD + 11);
  for (int i = 0; i < 4; i++) {
    int y = 62 + i * 34;
    draw_sprite_screen(i == s_menuSel ? SPR_menu_pause_btn_pink_normal : SPR_menu_pause_btn_blue_normal, 110, y, 0, 0.45f, false, 32, Z_HUD + 11);
    draw_sprite_screen(icons[i], 110, y, 0, 0.45f, false, 32, Z_HUD + 12);
    const SpriteDef *d = sprite_def(items[i]);
    draw_sprite_screen(items[i], 135 + d->ax * 0.25f, y - d->h / 2 + d->ay * 0.25f, 0, 1, false, i == s_menuSel ? 32 : 24, Z_HUD + 12);
  }
  center_sprite(T_DISCORD_GG_STI2D, 200, Z_HUD + 12);
  center_sprite(T_DISCORD_CJNW_YAMII, 216, Z_HUD + 12);
  gfx_render(0);
  if (in_pressed(K_BACK)) s_state = ST_PLAY;
  if (in_pressed(K_OK) || in_pressed(K_EXE)) {
    switch (s_menuSel) {
      case 0: s_state = ST_PLAY; break;
      case 1: load_level(s_level); s_state = ST_PLAY; break;
      case 2: s_state = ST_LEVELSEL; s_menuSel = s_levelInChapter; break;
      case 3: s_state = ST_TITLE; break;
    }
  }
}

static void complete_screen(float dt) {
  play_step(dt);
  draw_play_scene();
  const SpriteDef *d = sprite_def(SPR_victory_banner);
  draw_sprite_screen(SPR_victory_banner, 160, 70, 0, 320.0f / d->w > 1 ? 1 : 300.0f / d->w, false, 32, Z_HUD + 5);
  center_sprite(T_LEVEL_COMPLETE, 112, Z_HUD + 6);
  draw_time(s_completeTime, 125, 132);
  center_sprite(T_PRESS_OK, 170, Z_HUD + 6);
  gfx_render(0);
  if (in_pressed(K_OK) || in_pressed(K_EXE)) {
    const ChapterEntry *ch = &CHAPTERS[s_chapter];
    if (s_levelInChapter + 1 < ch->n) {
      s_levelInChapter++;
      if (load_level(ch->first + s_levelInChapter)) s_state = ST_PLAY; else s_state = ST_ERROR;
    } else { s_state = ST_LEVELSEL; s_menuSel = s_levelInChapter; }
  }
  if (in_pressed(K_BACK)) { s_state = ST_LEVELSEL; s_menuSel = s_levelInChapter; }
}

void game_init(void) {
  int sz;
  s_arena = plat_arena(&sz);
  s_arenaSize = sz;
  cam.zoom = 62.5f * 2.0f / 3.0f;
}

// one frame; returns false to quit
bool game_frame(float dt) {
  s_frame++;
  uint64_t k = plat_keys();
  IN.pressed = k & ~IN.down;
  IN.down = k;
  switch (s_state) {
    case ST_TITLE: title_screen(); break;
    case ST_CHARSEL: charsel_screen(); break;
    case ST_LEVELSEL: levelsel_screen(); break;
    case ST_PLAY:
      if (in_pressed(K_BACK)) { s_state = ST_PAUSE; s_menuSel = 0; break; }
      if (in_pressed(K_EXE)) { load_level(s_level); break; }
      play_step(dt);
      if (s_complete) { s_state = ST_COMPLETE; }
      draw_play_scene();
      gfx_render(level_bg_color());
      break;
    case ST_PAUSE: pause_screen(); break;
    case ST_COMPLETE: complete_screen(dt); break;
    case ST_ERROR:
      draw_menu_bg();
      center_sprite(T_LEVEL_TOO_BIG_FOR_RAM, 110, 3);
      gfx_render(0);
      if (in_pressed(K_BACK) || in_pressed(K_OK)) s_state = ST_LEVELSEL;
      break;
  }
  return !s_quit;
}

// test hook for the simulator
#ifndef PLATFORM_DEVICE
static void dbg_mass(void) {
  float m = 0, mv = 0;
  for (int b = 1; b < W.nb; b++) { if (W.bodies[b].tag == TAG_CHAR) m += W.bodies[b].mass; if (W.bodies[b].tag == TAG_VEH) mv += W.bodies[b].mass; }
  printf("char mass %.2f veh mass %.2f bodies %d/%d fix %d/%d joints %d/%d arena %d\n", m, mv, W.nb, W.maxb, W.nf, W.maxf, W.nj, W.maxj, s_arenaUsed);
}
#endif
void game_debug_start(int level) {
  for (int c = 0; c < NCHAPTERS; c++)
    if (level >= CHAPTERS[c].first && level < CHAPTERS[c].first + CHAPTERS[c].n) { s_chapter = c; s_levelInChapter = level - CHAPTERS[c].first; }
  if (load_level(level)) s_state = ST_PLAY; else s_state = ST_ERROR;
#ifndef PLATFORM_DEVICE
  if (getenv("HW_DEBUG")) dbg_mass();
#endif
}
