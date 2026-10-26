// Player character: ragdoll (original body plists + joint limits) + vehicles.
#include "game.h"
#include "gfx.h"
#include "level.h"
#include "../build/assets.h"
#include "../build/chardefs.h"
#include <string.h>

Player PL;

#define DEG (3.14159265f / 180.0f)
#define CHAR_GROUP (-1)
#define CHAR_CAT 0x104
#define CHAR_MASK 0x10E

static VI s_vpool[256];        // vertices of character / vehicle boxes
static int s_nv;

static const VI *box_verts(float hw, float hh) {
  if (s_nv + 4 > 256) s_nv = 0;
  VI *v = &s_vpool[s_nv]; s_nv += 4;
  int16_t x = (int16_t)lrintf(hw * 64), y = (int16_t)lrintf(hh * 64);
  v[0].x = -x; v[0].y = -y; v[1].x = x; v[1].y = -y; v[2].x = x; v[2].y = y; v[3].x = -x; v[3].y = y;
  return v;
}
static const VI *poly_verts(const float *p, int n, float cx, float cy) {
  if (s_nv + n > 256) s_nv = 0;
  VI *v = &s_vpool[s_nv]; s_nv += n;
  for (int i = 0; i < n; i++) { v[i].x = (int16_t)lrintf((p[2 * i] - cx) * 64); v[i].y = (int16_t)lrintf((p[2 * i + 1] - cy) * 64); }
  return v;
}

static const BodyDefSet *defset(const char *name) {
  for (int i = 0; i < NBODYDEFS; i++) if (!strcmp(BODYDEFS[i].name, name)) return &BODYDEFS[i];
  return &BODYDEFS[0];
}
static const BodyPartDef *part(const BodyDefSet *s, const char *name) {
  for (int i = 0; i < s->nparts; i++) if (!strcmp(s->parts[i].name, name)) return &s->parts[i];
  return 0;
}
static void anchor(const BodyDefSet *s, const char *name, float *x, float *y) {
  for (int i = 0; i < s->nanchors; i++) if (!strcmp(s->anchors[i].name, name)) { *x = s->anchors[i].x; *y = s->anchors[i].y; return; }
  *x = 0; *y = 0;
}

int sprite_find(const char *name) {
  for (int i = 0; i < ASSET_NSPR; i++) if (!strcmp(SPR_NAMES[i], name)) return i;
  return -1;
}

static float s_ox, s_oy;   // character origin (world)

static void char_filter(int f) { fix_filter(f, CHAR_CAT, CHAR_MASK, CHAR_GROUP); }

static int make_box_body(const BodyPartDef *d, float dens, int tag) {
  if (!d) return -1;
  int b = body_create(BT_DYNAMIC, s_ox + d->x, s_oy + d->y, d->rot);
  if (b < 0) return -1;
  int f;
  if (d->type == 1) f = fix_circle(b, 0, 0, d->hw, dens);
  else f = fix_poly(b, box_verts(d->hw, d->hh), 4, 0, 0, 0, dens);
  char_filter(f);
  if (f >= 0) { W.fix[f].tag = tag; W.fix[f].friction = 60; W.fix[f].restitution = 5; }
  body_finalize(b, 0);
  W.bodies[b].tag = tag;
  W.bodies[b].flags |= BF_KEEPAWAKE;
  return b;
}

// ------------------------------------------------------------------ ragdoll
static const char *const PART_NAMES[NPARTS] = { "chestShape", "pelvisShape", "headShape", "upperArm1Shape", "lowerArm1Shape",
  "upperArm2Shape", "lowerArm2Shape", "upperLeg1Shape", "lowerLeg1Shape", "upperLeg2Shape", "lowerLeg2Shape" };
static const char *const PART_SPR[NPARTS] = { "chest_1", "pelvis", "head_1", "upperArm1_1", "lowerArm1_1", "upperArm2_1",
  "lowerArm2_1", "upperLeg1_1", "lowerLeg1_1", "upperLeg2_1", "lowerLeg2_1" };
static const char *const PART_SPR2[NPARTS] = { "chest_2", "pelvis", "head_2", "upperArm1_2", "lowerArm1_2", "upperArm2_2",
  "lowerArm2_2", "upperLeg1_2", "lowerLeg1_2", "upperLeg2_2", "lowerLeg2_2" };
// joint: parent, child, anchor, limits (y-down degrees, from CharacterB2D::createJoints)
static const struct { uint8_t a, b; const char *anchor; float lo, hi; float brk; } RJ[NRJ] = {
  { P_CHEST, P_HEAD, "headAnchor", -20, 20, 560 },
  { P_CHEST, P_UARM1, "upperArmAnchor", -180, 60, 520 },
  { P_UARM1, P_LARM1, "lowerArm1Anchor", -160, 0, 480 },
  { P_CHEST, P_UARM2, "upperArmAnchor", -180, 60, 520 },
  { P_UARM2, P_LARM2, "lowerArm2Anchor", -160, 0, 480 },
  { P_CHEST, P_PELVIS, "pelvisAnchor", -5, 5, 1100 },
  { P_PELVIS, P_ULEG1, "upperLegAnchor", -150, 10, 800 },
  { P_ULEG1, P_LLEG1, "lowerLeg1Anchor", 0, 150, 720 },
  { P_PELVIS, P_ULEG2, "upperLegAnchor", -150, 10, 800 },
  { P_ULEG2, P_LLEG2, "lowerLeg2Anchor", 0, 150, 720 },
};

static void spr_name(char *buf, const char *prefix, const char *name) {
  strcpy(buf, prefix); strcat(buf, name);
}

static void ragdoll_create(Ragdoll *r, const char *set, const char *prefix, bool kid) {
  const BodyDefSet *s = defset(set);
  memset(r, 0, sizeof(*r));
  r->kid = kid;
  char buf[64];
  for (int p = 0; p < NPARTS; p++) {
    r->b[p] = make_box_body(part(s, PART_NAMES[p]), 1.0f, TAG_CHAR);
    if (r->b[p] >= 0) W.bodies[r->b[p]].user = (uint16_t)(p | (kid ? 0x100 : 0));
    spr_name(buf, prefix, PART_SPR[p]); r->spr[p] = sprite_find(buf);
    if (r->spr[p] < 0 && kid) {   // kid sprites have no variants
      const char *n = PART_SPR[p];
      spr_name(buf, prefix, n); buf[strlen(buf) - 2] = 0; r->spr[p] = sprite_find(buf);
    }
    spr_name(buf, prefix, PART_SPR2[p]); r->sprAlt[p] = sprite_find(buf);
    if (r->sprAlt[p] < 0) r->sprAlt[p] = r->spr[p];
  }
  for (int k = 0; k < 4; k++) {
    char n[20]; strcpy(n, "headChunk_1"); n[10] = (char)('1' + k);
    spr_name(buf, prefix, n); r->headChunk[k] = sprite_find(buf);
  }
  spr_name(buf, prefix, "chestChunk_1"); r->chestChunk = sprite_find(buf);
  spr_name(buf, prefix, "brain"); r->brain = sprite_find(buf);
  for (int j = 0; j < NRJ; j++) {
    float ax, ay; anchor(s, RJ[j].anchor, &ax, &ay);
    int A = r->b[RJ[j].a], B = r->b[RJ[j].b];
    if (A < 0 || B < 0) { r->j[j] = -1; continue; }
    r->j[j] = joint_revolute(A, B, s_ox + ax, s_oy + ay, true, RJ[j].lo * DEG, RJ[j].hi * DEG, true, 0, 2.0f, false);
    if (r->j[j] >= 0) { W.joints[r->j[j]].tag = TAG_CHAR; W.joints[r->j[j]].user = j; }
    r->target[j] = 0;
  }
}

static float joint_angle(int j) {
  const Joint *jt = &W.joints[j];
  return W.bodies[jt->bb].a - W.bodies[jt->ba].a - jt->ref;
}

static void ragdoll_pose(Ragdoll *r, bool alive, float strength) {
  for (int j = 0; j < NRJ; j++) {
    if (r->j[j] < 0 || (r->broken & (1 << j))) continue;
    Joint *jt = &W.joints[r->j[j]];
    if (!alive) { jt->speed = 0; jt->maxF = 0.15f; continue; }
    float err = r->target[j] - joint_angle(r->j[j]);
    jt->speed = clampf(err * 12.0f, -12, 12);
    jt->maxF = (j == J_WAIST || j == J_NECK ? 6.0f : 3.0f) * strength;
  }
}

static void bleed_at(float x, float y, int n) {
  particle_blood(x, y, 0, -1, n);
}

static void ragdoll_break(Ragdoll *r, int j) {
  if (r->j[j] < 0 || (r->broken & (1 << j))) return;
  Joint *jt = &W.joints[r->j[j]];
  float x, y; body_world_point(&W.bodies[jt->ba], jt->lax, jt->lay, &x, &y);
  joint_kill(r->j[j]);
  r->broken |= 1 << j;
  bleed_at(x, y, 10);
  PL.bleed += 60;
  if (j == J_NECK && !r->kid) player_kill();
  if (j == J_WAIST && !r->kid) player_kill();
  // when a limb is lost, the matching vehicle attachments go too
  for (int k = 0; k < PL.natt; k++) {
    if (PL.att[k] < 0) continue;
    Joint *a = &W.joints[PL.att[k]];
    if (a->flags & JF_DEAD) PL.att[k] = -1;
  }
}

#ifndef PLATFORM_DEVICE
#include <stdio.h>
#include <stdlib.h>
#endif
static void ragdoll_check(Ragdoll *r) {
#ifndef PLATFORM_DEVICE
  if (getenv("HW_DEBUG") && !r->kid) {
    static int fr; fr++;
    if (fr < 400 && fr % 4 == 0) { printf("f%d:", fr); for (int j = 0; j < NRJ; j++) if (r->j[j] >= 0) printf(" %5.1f", W.joints[r->j[j]].force); printf(" | att"); for (int k = 0; k < PL.natt; k++) if (PL.att[k] >= 0) printf(" %5.1f", W.joints[PL.att[k]].force); printf("\n"); }
  }
#endif
  for (int j = 0; j < NRJ; j++) {
    if (r->j[j] < 0 || (r->broken & (1 << j))) continue;
    Joint *jt = &W.joints[r->j[j]];
    if (jt->flags & JF_DEAD) { r->broken |= 1 << j; continue; }
    float lim = RJ[j].brk * (r->kid ? 0.5f : 1.0f);
    if (jt->force > lim) ragdoll_break(r, j);
  }
}

// ------------------------------------------------------------------ vehicles
enum { V_SEGWAY, V_BIKE, V_WHEELCHAIR, V_CART, V_POGO };
enum { VB_CHASSIS, VB_WHEEL1, VB_WHEEL2, VB_GEAR, VB_ROD, VB_EXTRA };

static float s_vdens = 5.0f;   // vehicle density (original: 5, motor cart: 4)

static int add_part_fixture(int b, const BodyPartDef *d, float bx, float by, float dens) {
  if (!d) return -1;
  dens *= s_vdens;
  int f;
  if (d->type == 1) f = fix_circle(b, d->x - bx, d->y - by, d->hw, dens);
  else if (d->type == 2) f = fix_poly(b, poly_verts(d->v, d->nv, bx, by), d->nv, 0, 0, 0, dens);
  else f = fix_poly(b, box_verts(d->hw, d->hh), 4, d->x - bx, d->y - by, d->rot, dens);
  char_filter(f);
  if (f >= 0) { W.fix[f].tag = TAG_VEH; W.fix[f].friction = 80; }
  return f;
}

static int make_wheel(const BodyPartDef *d, float dens) {
  if (!d) return -1;
  dens *= s_vdens;
  int b = body_create(BT_DYNAMIC, s_ox + d->x, s_oy + d->y, 0);
  int f = fix_circle(b, 0, 0, d->hw, dens);
  char_filter(f);
  if (f >= 0) { W.fix[f].tag = TAG_VEH; W.fix[f].friction = 150; W.fix[f].restitution = 5; }
  body_finalize(b, 0);
  W.bodies[b].tag = TAG_VEH; W.bodies[b].flags |= BF_KEEPAWAKE;
  return b;
}

static int attach(int a, int b, float wx, float wy, float lo, float hi) {
  int j = joint_revolute(a, b, wx, wy, true, lo, hi, false, 0, 0, false);
  if (j >= 0 && PL.natt < 8) { PL.att[PL.natt++] = j; W.joints[j].tag = TAG_VEH; }
  return j;
}

static void build_vehicle(void) {
  Ragdoll *r = &PL.rd;
  memset(PL.vb, -1, sizeof(PL.vb));
  memset(PL.vj, -1, sizeof(PL.vj));
  PL.natt = 0;
  float ax, ay;
  s_vdens = PL.veh == V_CART ? 4.0f : 5.0f;
  switch (PL.veh) {
    case V_SEGWAY: {
      // PersonalTransporter: stand with fixed rotation, wheel motor, frame on a +-15 deg motorized joint
      const BodyDefSet *s = defset("personal_transporter");
      const BodyPartDef *st = part(s, "standShape"), *fr = part(s, "frameShape");
      int c = body_create(BT_DYNAMIC, s_ox + st->x, s_oy + st->y, 0);
      W.bodies[c].flags |= BF_FIXEDROT;
      add_part_fixture(c, st, st->x, st->y, 1.0f);
      body_finalize(c, 0); W.bodies[c].tag = TAG_VEH; W.bodies[c].flags |= BF_KEEPAWAKE;
      PL.vb[VB_CHASSIS] = c;
      int fb = body_create(BT_DYNAMIC, s_ox + fr->x, s_oy + fr->y, 0);
      add_part_fixture(fb, fr, fr->x, fr->y, 1.0f);
      add_part_fixture(fb, part(s, "handleShape"), fr->x, fr->y, 1.0f);
      body_finalize(fb, 0); W.bodies[fb].tag = TAG_VEH; W.bodies[fb].flags |= BF_KEEPAWAKE;
      PL.vb[VB_EXTRA] = fb;
      int w = make_wheel(part(s, "wheelShape"), 1.0f);
      PL.vb[VB_WHEEL1] = w;
      const BodyPartDef *wd = part(s, "wheelShape");
      PL.vj[0] = joint_revolute(c, w, s_ox + wd->x, s_oy + wd->y, false, 0, 0, false, 0, 20, false);
      anchor(s, "frameAnchor", &ax, &ay);
      PL.vj[2] = joint_revolute(c, fb, s_ox + ax, s_oy + ay, true, -0.2618f, 0.2618f, true, 0, 60, false);
      anchor(s, "footAnchor", &ax, &ay);
      attach(c, r->b[P_LLEG1], s_ox + ax, s_oy + ay, -0.3f, 0.3f);
      attach(c, r->b[P_LLEG2], s_ox + ax, s_oy + ay, -0.3f, 0.3f);
      anchor(s, "handleAnchor", &ax, &ay);
      attach(fb, r->b[P_LARM1], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      attach(fb, r->b[P_LARM2], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      break;
    }
    case V_BIKE: {
      const BodyDefSet *s = defset("road_bike");
      const BodyPartDef *fr = part(s, "frame");
      float cx = 0, cy = 0;
      for (int i = 0; i < fr->nv; i++) { cx += fr->v[2 * i]; cy += fr->v[2 * i + 1]; }
      cx /= fr->nv; cy /= fr->nv;
      int c = body_create(BT_DYNAMIC, s_ox + cx, s_oy + cy, 0);
      add_part_fixture(c, fr, cx, cy, 1.0f);
      add_part_fixture(c, part(s, "seat"), cx, cy, 1.0f);
      add_part_fixture(c, part(s, "fork"), cx, cy, 1.0f);
      body_finalize(c, 0); W.bodies[c].tag = TAG_VEH; W.bodies[c].flags |= BF_KEEPAWAKE;
      PL.vb[VB_CHASSIS] = c;
      const BodyPartDef *fw = part(s, "frontWheelShape"), *bw = part(s, "backWheelShape");
      PL.vb[VB_WHEEL1] = make_wheel(bw, 1.0f);
      PL.vb[VB_WHEEL2] = make_wheel(fw, 1.0f);
      PL.vj[0] = joint_revolute(c, PL.vb[VB_WHEEL1], s_ox + bw->x, s_oy + bw->y, false, 0, 0, false, 0, 6, false);
      PL.vj[1] = joint_revolute(c, PL.vb[VB_WHEEL2], s_ox + fw->x, s_oy + fw->y, false, 0, 0, false, 0, 0, false);
      anchor(s, "handleAnchor", &ax, &ay);
      attach(c, r->b[P_LARM1], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      attach(c, r->b[P_LARM2], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      // pelvis on the seat
      {
        const BodyPartDef *se = part(s, "seat");
        float sx = (se->v[0] + se->v[2] + se->v[4]) / 3, sy = (se->v[1] + se->v[3] + se->v[5]) / 3;
        (void)sx; (void)sy;
        float px = W.bodies[r->b[P_PELVIS]].cx, py = W.bodies[r->b[P_PELVIS]].cy;
        attach(c, r->b[P_PELVIS], px, py + 0.1f, -0.6f, 0.6f);
      }
      anchor(s, "gear1Anchor", &ax, &ay);
      attach(c, r->b[P_LLEG1], s_ox + ax, s_oy + ay, -3.14f, 3.14f);
      anchor(s, "gear2Anchor", &ax, &ay);
      attach(c, r->b[P_LLEG2], s_ox + ax, s_oy + ay, -3.14f, 3.14f);
      // kid on the child seat
      {
        Ragdoll *k = &PL.kid;
        float kx = W.bodies[k->b[P_PELVIS]].cx, ky = W.bodies[k->b[P_PELVIS]].cy;
        PL.nkidAtt = 0;
        int j = joint_revolute(c, k->b[P_PELVIS], kx, ky + 0.05f, true, -0.4f, 0.4f, false, 0, 0, false);
        PL.kidAtt[PL.nkidAtt++] = j;
        j = joint_revolute(r->b[P_CHEST], k->b[P_LARM1], W.bodies[k->b[P_LARM1]].cx + 0.1f, W.bodies[k->b[P_LARM1]].cy, true, -1.5f, 1.5f, false, 0, 0, false);
        PL.kidAtt[PL.nkidAtt++] = j;
      }
      break;
    }
    case V_WHEELCHAIR: {
      const BodyDefSet *s = defset("wheelchair");
      const BodyPartDef *c1 = part(s, "chair1Shape");
      int c = body_create(BT_DYNAMIC, s_ox + c1->x, s_oy + c1->y, 0);
      add_part_fixture(c, c1, c1->x, c1->y, 0.2f);
      add_part_fixture(c, part(s, "chair2Shape"), c1->x, c1->y, 1.0f);
      add_part_fixture(c, part(s, "chair3Shape"), c1->x, c1->y, 1.0f);
      body_finalize(c, 0); W.bodies[c].tag = TAG_VEH; W.bodies[c].flags |= BF_KEEPAWAKE;
      PL.vb[VB_CHASSIS] = c;
      const BodyPartDef *bw = part(s, "bigWheelShape"), *sw = part(s, "smallWheelShape");
      PL.vb[VB_WHEEL1] = make_wheel(bw, 1.0f);
      PL.vb[VB_WHEEL2] = make_wheel(sw, 1.0f);
      PL.vj[0] = joint_revolute(c, PL.vb[VB_WHEEL1], s_ox + bw->x, s_oy + bw->y, false, 0, 0, false, 0, 8, false);
      PL.vj[1] = joint_revolute(c, PL.vb[VB_WHEEL2], s_ox + sw->x, s_oy + sw->y, false, 0, 0, false, 0, 0, false);
      float px = W.bodies[r->b[P_PELVIS]].cx, py = W.bodies[r->b[P_PELVIS]].cy;
      attach(c, r->b[P_PELVIS], px, py, -0.3f, 0.3f);
      float lx = W.bodies[r->b[P_ULEG1]].cx, ly = W.bodies[r->b[P_ULEG1]].cy;
      attach(c, r->b[P_ULEG1], lx, ly, -0.3f, 0.3f);
      attach(c, r->b[P_ULEG2], lx, ly, -0.3f, 0.3f);
      break;
    }
    case V_CART: {
      const BodyDefSet *s = defset("motor_cart");
      const BodyPartDef *ba = part(s, "baseShape");
      int c = body_create(BT_DYNAMIC, s_ox + ba->x, s_oy + ba->y, 0);
      static const char *const cp[] = { "baseShape", "seatShape", "shaftShape", "handleShape", "cartShape", "rear", "front" };
      for (int i = 0; i < 7; i++) add_part_fixture(c, part(s, cp[i]), ba->x, ba->y, 1.0f);
      body_finalize(c, 0); W.bodies[c].tag = TAG_VEH; W.bodies[c].flags |= BF_KEEPAWAKE;
      PL.vb[VB_CHASSIS] = c;
      const BodyPartDef *bw = part(s, "backWheelShape"), *fw = part(s, "frontWheelShape");
      PL.vb[VB_WHEEL1] = make_wheel(bw, 1.0f);
      PL.vb[VB_WHEEL2] = make_wheel(fw, 1.0f);
      PL.vj[0] = joint_revolute(c, PL.vb[VB_WHEEL1], s_ox + bw->x, s_oy + bw->y, false, 0, 0, false, 0, 5, false);
      PL.vj[1] = joint_revolute(c, PL.vb[VB_WHEEL2], s_ox + fw->x, s_oy + fw->y, false, 0, 0, false, 0, 5, false);
      anchor(s, "handleAnchor", &ax, &ay);
      attach(c, r->b[P_LARM1], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      attach(c, r->b[P_LARM2], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      float px = W.bodies[r->b[P_PELVIS]].cx, py = W.bodies[r->b[P_PELVIS]].cy;
      attach(c, r->b[P_PELVIS], px, py + 0.08f, -0.4f, 0.4f);
      float fx = W.bodies[r->b[P_LLEG1]].cx, fy = W.bodies[r->b[P_LLEG1]].cy + 0.25f;
      attach(c, r->b[P_LLEG1], fx, fy, -0.5f, 0.5f);
      attach(c, r->b[P_LLEG2], fx, fy, -0.5f, 0.5f);
      // groceries
      for (int i = 0; i < 6; i++) {
        char n[8] = "box0"; n[3] = '0' + i;
        const BodyPartDef *d = part(s, n);
        if (!d) continue;
        int g = body_create(BT_DYNAMIC, s_ox + d->x, s_oy + d->y, d->rot);
        int f = fix_poly(g, box_verts(d->hw, d->hh), 4, 0, 0, 0, 0.3f);
        fix_filter(f, 8, 0xFFFF, 0);
        body_finalize(g, 0);
        W.bodies[g].tag = TAG_DEBRIS; W.bodies[g].user = i;
        if (i < 3) PL.vb[VB_EXTRA + i] = g;
      }
      break;
    }
    case V_POGO: {
      const BodyDefSet *s = defset("pogo_stick");
      const BodyPartDef *fr = part(s, "frameShape"), *rd = part(s, "rodShape");
      int c = body_create(BT_DYNAMIC, s_ox + fr->x, s_oy + fr->y, fr->rot);
      int f = fix_poly(c, box_verts(fr->hw, fr->hh), 4, 0, 0, 0, 5.0f); char_filter(f);
      body_finalize(c, 0); W.bodies[c].tag = TAG_VEH; W.bodies[c].flags |= BF_KEEPAWAKE;
      PL.vb[VB_CHASSIS] = c;
      int rb = body_create(BT_DYNAMIC, s_ox + rd->x, s_oy + rd->y, rd->rot);
      f = fix_poly(rb, box_verts(rd->hw, rd->hh), 4, 0, 0, 0, 5.0f); char_filter(f);
      if (f >= 0) { W.fix[f].friction = 150; W.fix[f].tag = TAG_VEH; }
      body_finalize(rb, 0); W.bodies[rb].tag = TAG_VEH; W.bodies[rb].flags |= BF_KEEPAWAKE;
      PL.vb[VB_ROD] = rb;
      float axx = -sinf(fr->rot), axy = cosf(fr->rot);   // pogo axis (pointing down)
      PL.vj[0] = joint_prismatic(c, rb, W.bodies[rb].cx, W.bodies[rb].cy, axx, axy, true, -0.05f, 0.25f, true, 0, 60, false);
      anchor(s, "handleAnchor", &ax, &ay);
      attach(c, r->b[P_LARM1], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      attach(c, r->b[P_LARM2], s_ox + ax, s_oy + ay, -1.5f, 1.5f);
      anchor(s, "footAnchor", &ax, &ay);
      attach(c, r->b[P_LLEG1], s_ox + ax, s_oy + ay, -0.4f, 0.4f);
      attach(c, r->b[P_LLEG2], s_ox + ax, s_oy + ay, -0.4f, 0.4f);
      break;
    }
  }
}

// ------------------------------------------------------------------ create
void player_create(int chr, float sx, float sy) {
  memset(&PL, 0, sizeof(PL));
  s_nv = 0;
  PL.chr = chr;
  memset(PL.att, -1, sizeof(PL.att));
  memset(PL.kidAtt, -1, sizeof(PL.kidAtt));
  const char *set, *prefix; float offx, offy;
  switch (chr) {
    case 1: set = "wheelchair_guy_wheelchair"; prefix = "wheelchair_guy_"; PL.veh = V_WHEELCHAIR; offx = -0.08f; offy = 1.08f; break;
    case 3: set = "irresponsible_dad_road_bike"; prefix = "irresponsible_dad_"; PL.veh = V_BIKE; offx = -0.08f; offy = 1.792f; break;
    case 4: set = "effective_shopper_motor_cart"; prefix = "effective_shopper_"; PL.veh = V_CART; offx = -0.48f; offy = 1.256f; break;
    case 9: set = "pogo_stick_guy_pogo_stick"; prefix = "pogo_stick_guy_"; PL.veh = V_POGO; offx = 0; offy = 1.664f; break;
    default: set = "business_guy_personal_transporter"; prefix = "business_guy_"; PL.veh = V_SEGWAY; offx = 0; offy = 1.792f; PL.chr = 2; break;
  }
  // the level start point is the character origin (offsets only matter for the editor)
  (void)offx; (void)offy;
  s_ox = sx; s_oy = sy;
  ragdoll_create(&PL.rd, set, prefix, false);
  if (PL.veh == V_BIKE) ragdoll_create(&PL.kid, "irresponsible_dad_kid_road_bike", "irresponsible_dad_kid_", true);
  else for (int i = 0; i < NPARTS; i++) PL.kid.b[i] = -1;
  build_vehicle();
  PL.x = sx; PL.y = sy - 1;
}

bool player_is_char_body(int b) {
  if (b <= 0) return false;
  return W.bodies[b].tag == TAG_CHAR;
}
bool player_owns_body(int b) {
  if (b <= 0) return false;
  return W.bodies[b].tag == TAG_CHAR || W.bodies[b].tag == TAG_VEH;
}

void player_kill(void) {
  if (PL.dead) return;
  PL.dead = true;
  PL.deadTimer = 0;
}

void player_impale(int b, int spikeBody, float wx, float wy) {
  if (PL.rd.impaled > 6) return;
  int j = joint_weld(spikeBody, b, wx, wy);
  if (j >= 0) { W.joints[j].tag = TAG_DEBRIS; PL.rd.impaled++; }
  particle_blood(wx, wy, 0, -2, 8);
  PL.bleed += 40;
}

static void eject(void) {
  for (int k = 0; k < PL.natt; k++) if (PL.att[k] >= 0) { joint_kill(PL.att[k]); PL.att[k] = -1; }
  PL.ejected = true;
}

// ------------------------------------------------------------------ update
static void set_motor(int j, float speed, float torque) {
  if (j < 0) return;
  Joint *jt = &W.joints[j];
  if (jt->flags & JF_DEAD) return;
  jt->flags |= JF_MOTOR; jt->speed = speed; jt->maxF = torque;
  body_wake(jt->ba); body_wake(jt->bb);
}
static void motor_off(int j) {
  if (j < 0) return;
  W.joints[j].flags &= ~JF_MOTOR;
}

static bool wheel_on_ground(int b) {
  if (b < 0) return false;
  for (int i = 0; i < W.nc; i++) {
    const Contact *c = &W.contacts[i];
    if (c->flags & 0x40) continue;
    int ba = W.fix[c->fa].body;
    int bb = (c->fb & 0x8000) ? 0 : W.fix[c->fb].body;
    if (ba == b && !player_owns_body(bb)) return true;
    if (bb == b && !player_owns_body(ba)) return true;
  }
  return false;
}

static int s_chassisTarget;

void player_update(float dt) {
  Ragdoll *r = &PL.rd;
  bool alive = !PL.dead;
  bool control = alive && !PL.ejected;
  int c = PL.vb[VB_CHASSIS];
  float leanIn = 0;
  if (control) {
    if (in_down(K_LEFT)) leanIn = -1;
    if (in_down(K_RIGHT)) leanIn = 1;
  }
  PL.lean += (leanIn - PL.lean) * clampf(dt * 8, 0, 1);
  (void)s_chassisTarget;
  // pose targets
  for (int j = 0; j < NRJ; j++) r->target[j] = 0;
  if (alive) {
    if (PL.lean > 0) {   // lean forward
      r->target[J_WAIST] = 0; r->target[J_SHO1] = r->target[J_SHO2] = -0.3f * PL.lean;
      r->target[J_ELB1] = r->target[J_ELB2] = -0.5f * PL.lean;
      r->target[J_HIP1] = r->target[J_HIP2] = -0.3f * PL.lean;
    } else if (PL.lean < 0) {
      float l = -PL.lean;
      r->target[J_ELB1] = r->target[J_ELB2] = 0.3f * l;
      r->target[J_HIP1] = r->target[J_HIP2] = 0.25f * l;
      r->target[J_KNEE1] = r->target[J_KNEE2] = 0.2f * l;
    }
  }
  ragdoll_pose(r, alive, PL.ejected ? 0.5f : 1.0f);
  if (PL.kid.b[0] >= 0) ragdoll_pose(&PL.kid, alive && !(PL.kid.broken & 1), 0.6f);

  if (!control) {
    for (int i = 0; i < 2; i++) motor_off(PL.vj[i]);
    if (alive && PL.ejected) {
      // limited air control like the original: arms flail with lean
    }
    return;
  }
  bool fwd = in_down(K_UP), back = in_down(K_DOWN), spec = in_down(K_OK);
  if (in_pressed(K_BACKSPACE)) { eject(); return; }
  if (PL.jumpCool > 0) PL.jumpCool -= dt;
  switch (PL.veh) {
    case V_SEGWAY:
      if (fwd) set_motor(PL.vj[0], 35, 3.5f); else if (back) set_motor(PL.vj[0], -35, 3.5f); else set_motor(PL.vj[0], 0, 0.6f);
      if (PL.vj[2] >= 0) {
        float target = 0.2618f * PL.lean;
        float err = target - joint_angle(PL.vj[2]);
        set_motor(PL.vj[2], clampf(err * 10, -6, 6), 60);
      }
      if (spec && PL.jumpCool <= 0 && wheel_on_ground(PL.vb[VB_WHEEL1])) {
        PL.jumpCool = 0.6f;
        for (int p = 0; p < NPARTS; p++) if (r->b[p] >= 0) body_apply_impulse(r->b[p], 0, -W.bodies[r->b[p]].mass * 6.0f, W.bodies[r->b[p]].cx, W.bodies[r->b[p]].cy);
        body_apply_impulse(c, 0, -W.bodies[c].mass * 6.0f, W.bodies[c].cx, W.bodies[c].cy);
        body_apply_impulse(PL.vb[VB_WHEEL1], 0, -W.bodies[PL.vb[VB_WHEEL1]].mass * 6.0f, W.bodies[PL.vb[VB_WHEEL1]].cx, W.bodies[PL.vb[VB_WHEEL1]].cy);
      }
      break;
    case V_BIKE:
      if (spec) set_motor(PL.vj[0], 0, 40), set_motor(PL.vj[1], 0, 40);
      else {
        motor_off(PL.vj[1]);
        if (fwd) set_motor(PL.vj[0], 26, 15); else if (back) set_motor(PL.vj[0], -14, 10); else motor_off(PL.vj[0]);
      }
      break;
    case V_WHEELCHAIR:
      if (fwd) set_motor(PL.vj[0], 15, 10); else if (back) set_motor(PL.vj[0], -15, 10); else set_motor(PL.vj[0], 0, 1.0f);
      if (spec) {
        Body *cb = &W.bodies[c];
        float f = 90.0f;
        body_apply_force(c, cb->c * f, cb->s * f, cb->cx, cb->cy, dt);
        PL.jet = 0.15f;
        if ((plat_frame_counter() & 1) == 0) particle_spawn(SPR_spark_particle_0, cb->cx - cb->c * 0.5f, cb->cy - cb->s * 0.5f + 0.2f, -cb->c * 4, -cb->s * 4, 0.3f, 0);
      }
      break;
    case V_CART:
      if (fwd) { set_motor(PL.vj[0], 45, 2.6f); set_motor(PL.vj[1], 45, 2.6f); }
      else if (back) { set_motor(PL.vj[0], -30, 2.2f); set_motor(PL.vj[1], -30, 2.2f); }
      else { motor_off(PL.vj[0]); motor_off(PL.vj[1]); }
      if (spec && PL.jumpCool <= 0 && (wheel_on_ground(PL.vb[VB_WHEEL1]) || wheel_on_ground(PL.vb[VB_WHEEL2]))) {
        PL.jumpCool = 0.7f;
        for (int p = 0; p < NPARTS; p++) if (r->b[p] >= 0) body_apply_impulse(r->b[p], 0, -W.bodies[r->b[p]].mass * 6.5f, W.bodies[r->b[p]].cx, W.bodies[r->b[p]].cy);
        for (int i = 0; i < 3; i++) if (PL.vb[i] >= 0) body_apply_impulse(PL.vb[i], 0, -W.bodies[PL.vb[i]].mass * 6.5f, W.bodies[PL.vb[i]].cx, W.bodies[PL.vb[i]].cy);
      }
      break;
    case V_POGO: {
      Joint *pj = PL.vj[0] >= 0 ? &W.joints[PL.vj[0]] : 0;
      if (pj) {
        if (fwd) { PL.pogoCharge = clampf(PL.pogoCharge + dt * 2, 0, 1); pj->speed = -1.0f; pj->maxF = 80; }
        else if (PL.pogoCharge > 0) {
          // release: spring pushes the rod down -> jump
          pj->speed = 12 + 10 * PL.pogoCharge; pj->maxF = 400 + 600 * PL.pogoCharge;
          PL.pogoCharge -= dt * 4;
          if (PL.pogoCharge < 0) PL.pogoCharge = 0;
        } else { pj->speed = 2.0f; pj->maxF = 40; }
        body_wake(pj->ba); body_wake(pj->bb);
      }
      break;
    }
  }
  // lean torque on the vehicle (and the rider) + mild self righting like the original vehicles
  if (c >= 0 && PL.veh != V_SEGWAY) {
    Body *cb = &W.bodies[c];
    float tq = (PL.veh == V_POGO ? 2.0f : PL.veh == V_BIKE ? 10.0f : 8.0f) * leanIn;
    if (PL.veh == V_WHEELCHAIR || PL.veh == V_CART || PL.veh == V_POGO || PL.veh == V_BIKE) {
      float a = atan2f(cb->s, cb->c);
      float k = PL.veh == V_POGO ? 1.5f : PL.veh == V_BIKE ? 6.0f : 14.0f;
      float lim = PL.veh == V_POGO ? 3.0f : 14.0f;
      if (leanIn == 0) tq += clampf(-k * a - 1.5f * cb->w, -lim, lim);
      else tq += clampf(-1.0f * cb->w, -5, 5);
      // motor reaction compensation (the rider's weight keeps the chair down)
      if (PL.veh == V_WHEELCHAIR && fwd) tq += 6.0f;
    }
    if (tq != 0) {
      // distribute the torque on the whole rig so it rotates as one piece
      float I = 0;
      for (int b = 1; b < W.nb; b++) if (player_owns_body(b) && W.bodies[b].invM > 0) {
        Body *bb = &W.bodies[b];
        float dx = bb->cx - cb->cx, dy = bb->cy - cb->cy;
        I += bb->mass * (dx * dx + dy * dy) + (bb->invI > 0 ? 1.0f / bb->invI : 0);
      }
      if (I > 0) {
        float dw = tq / I * dt;
        for (int b = 1; b < W.nb; b++) if (player_owns_body(b) && W.bodies[b].invM > 0) {
          Body *bb = &W.bodies[b];
          if (W.bodies[b].tag == TAG_CHAR && PL.ejected) continue;
          float dx = bb->cx - cb->cx, dy = bb->cy - cb->cy;
          bb->vx += -dy * dw; bb->vy += dx * dw; bb->w += dw;
        }
      }
      body_wake(c);
    }
  }
}

void player_after_step(float dt) {
  ragdoll_check(&PL.rd);
  if (PL.kid.b[0] >= 0) ragdoll_check(&PL.kid);
  // attachment breaking = ejection
  int alive = 0;
  for (int k = 0; k < PL.natt; k++) {
    if (PL.att[k] < 0) continue;
    Joint *j = &W.joints[PL.att[k]];
    if (j->flags & JF_DEAD) { PL.att[k] = -1; continue; }
    if (j->force > 420) { joint_kill(PL.att[k]); PL.att[k] = -1; continue; }
    alive++;
  }
  if (alive == 0 && PL.natt > 0) PL.ejected = true;
  // head impacts
  Ragdoll *r = &PL.rd;
  for (int i = 0; i < W.nc; i++) {
    const Contact *c = &W.contacts[i];
    if (c->flags & 0x40) continue;
    int ba = W.fix[c->fa].body;
    int bb = (c->fb & 0x8000) ? 0 : W.fix[c->fb].body;
    float imp = c->impulse / dt;   // force
    for (int s = 0; s < 2; s++) {
      int b = s ? bb : ba, o = s ? ba : bb;
      if (b != r->b[P_HEAD] || player_owns_body(o) || r->headGone) continue;
      if (imp > 480) {
        r->headGone = 1;
        float x = W.bodies[b].cx, y = W.bodies[b].cy;
        for (int k = 0; k < 4; k++) if (r->headChunk[k] >= 0) particle_spawn(r->headChunk[k], x, y, frand() * 6 - 3, -frand() * 5, 2.5f, frand() * 10 - 5);
        if (r->brain >= 0) particle_spawn(r->brain, x, y, frand() * 2 - 1, -3, 2.5f, 3);
        particle_blood(x, y, 0, -3, 20);
        W.fix[W.bodies[b].firstFix].flags |= FXF_DEAD;
        player_kill();
      }
    }
  }
  // bleeding
  if (PL.bleed > 0 && (plat_frame_counter() % 4) == 0) {
    PL.bleed--;
    int p = plat_random() % NPARTS;
    if (r->b[p] >= 0 && (r->broken & 0x3FF)) particle_blood(W.bodies[r->b[p]].cx, W.bodies[r->b[p]].cy, 0, 0, 1);
  }
  int brokenCount = 0;
  for (int j = 0; j < NRJ; j++) if (r->broken & (1 << j)) brokenCount++;
  if (brokenCount >= 4) player_kill();
  // focus
  int fb = r->b[P_CHEST] >= 0 ? r->b[P_CHEST] : r->b[P_PELVIS];
  if (fb >= 0) { PL.x = W.bodies[fb].cx; PL.y = W.bodies[fb].cy; PL.vx = W.bodies[fb].vx; PL.vy = W.bodies[fb].vy; }
  if (PL.dead) PL.deadTimer += dt;
  if (PL.jet > 0) PL.jet -= dt;
}

// ------------------------------------------------------------------ drawing
static void draw_body_sprite(int b, int spr, float lx, float ly, float rot, int z) {
  if (b < 0 || spr < 0) return;
  const Body *bd = &W.bodies[b];
  if (bd->flags & BF_DEAD) return;
  float ox, oy; body_origin(bd, &ox, &oy);
  float wx = ox + bd->c * lx - bd->s * ly, wy = oy + bd->s * lx + bd->c * ly;
  draw_sprite_world(spr, wx, wy, bd->a + rot, 1, false, 32, z);
}

static void draw_ragdoll(Ragdoll *r, int zbase) {
  static const uint8_t back[] = { P_UARM2, P_LARM2, P_ULEG2, P_LLEG2 };
  static const uint8_t mid[] = { P_PELVIS, P_CHEST, P_HEAD };
  static const uint8_t front[] = { P_ULEG1, P_LLEG1, P_UARM1, P_LARM1 };
  int z = zbase;
  for (int i = 0; i < 4; i++) draw_body_sprite(r->b[back[i]], r->spr[back[i]], 0, 0, 0, z++);
  z = zbase + 20;
  for (int i = 0; i < 3; i++) {
    int p = mid[i];
    if (p == P_HEAD && r->headGone) continue;
    int s = r->spr[p];
    if (p == P_CHEST && (r->broken & (1 << J_NECK))) s = r->sprAlt[p];
    draw_body_sprite(r->b[p], s, 0, 0, 0, z++);
  }
  for (int i = 0; i < 4; i++) draw_body_sprite(r->b[front[i]], r->spr[front[i]], 0, 0, 0, z++);
}

static void draw_wheel(int b, int spr, int z) { draw_body_sprite(b, spr, 0, 0, 0, z); }

void player_draw(void) {
  int z = Z_CHAR;
  Ragdoll *r = &PL.rd;
  // far limbs first, then vehicle, then the body
  switch (PL.veh) {
    case V_SEGWAY: {
      const BodyDefSet *s = defset("personal_transporter");
      const BodyPartDef *st = part(s, "standShape"), *fr = part(s, "frameShape"), *wh = part(s, "wheelShape");
      draw_ragdoll(r, z);
      draw_body_sprite(PL.vb[VB_EXTRA], SPR_personal_transporter_frame, 0, 0, fr->rot, z + 10);
      draw_wheel(PL.vb[VB_WHEEL1], SPR_personal_transporter_wheel, z + 11);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_personal_transporter_wheelCover, wh->x - st->x, wh->y - st->y, 0, z + 12);
      break;
    }
    case V_BIKE: {
      draw_ragdoll(&PL.kid, z);
      draw_wheel(PL.vb[VB_WHEEL1], SPR_road_bike_wheel, z + 8);
      draw_wheel(PL.vb[VB_WHEEL2], SPR_road_bike_wheel, z + 8);
      const BodyDefSet *s = defset("road_bike");
      const BodyPartDef *fr = part(s, "frame");
      float cx = 0, cy = 0;
      for (int i = 0; i < fr->nv; i++) { cx += fr->v[2 * i]; cy += fr->v[2 * i + 1]; }
      cx /= fr->nv; cy /= fr->nv;
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_road_bike_childSeat, -0.62f - cx, 0.85f - cy, 0, z + 9);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_road_bike_frame, 0.06f - cx, 0.85f - cy, 0, z + 9);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_road_bike_fork, 0.56f - cx, 0.76f - cy, 0, z + 9);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_road_bike_seat, -0.22f - cx, 0.48f - cy, 0, z + 9);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_road_bike_gear, -0.04f - cx, 1.11f - cy, 0, z + 9);
      draw_ragdoll(r, z + 30);
      break;
    }
    case V_WHEELCHAIR: {
      const BodyDefSet *s = defset("wheelchair");
      const BodyPartDef *c1 = part(s, "chair1Shape");
      draw_wheel(PL.vb[VB_WHEEL2], SPR_wheelchair_frontWheel, z);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_wheelchair_frame, 0.0f, 0.0f, 0, z + 1);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_wheelchair_jet, -0.36f, -0.1f, 0, z + 2);
      if (PL.jet > 0) draw_body_sprite(PL.vb[VB_CHASSIS], SPR_wheelchair_flame, -0.75f, -0.1f, 0, z + 2);
      draw_ragdoll(r, z + 5);
      draw_wheel(PL.vb[VB_WHEEL1], SPR_wheelchair_wheel, z + 40);
      (void)c1;
      break;
    }
    case V_CART: {
      const BodyDefSet *s = defset("motor_cart");
      const BodyPartDef *ba = part(s, "baseShape");
      draw_ragdoll(r, z);
      for (int i = 0; i < 3; i++) {
        int g = PL.vb[VB_EXTRA + i];
        if (g >= 0) draw_body_sprite(g, SPR_motor_cart_box_0 + i, 0, 0, 0, z + 9);
      }
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_motor_cart_cart, 1.17f - ba->x, 0.62f - ba->y, 0, z + 10);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_motor_cart_frame, 0.40f - ba->x, 0.70f - ba->y, 0, z + 10);
      draw_wheel(PL.vb[VB_WHEEL1], SPR_motor_cart_wheel, z + 11);
      draw_wheel(PL.vb[VB_WHEEL2], SPR_motor_cart_wheel, z + 11);
      break;
    }
    case V_POGO:
      draw_body_sprite(PL.vb[VB_ROD], SPR_pogo_stick_rod, 0, 0, 0, z);
      draw_ragdoll(r, z + 2);
      draw_body_sprite(PL.vb[VB_CHASSIS], SPR_pogo_stick_frame, 0, 0, 0, z + 30);
      break;
  }
}
