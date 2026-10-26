// Compact 2D rigid body physics: Box2D-lite style sequential impulses with
// warm starting, polygon clipping manifolds, revolute/prismatic/weld/rope joints,
// island sleeping and a static (flash resident) world.
#include "phys.h"
#include <string.h>

World W;

typedef struct { float x, y; } V2;
typedef struct { V2 v[8]; V2 n[8]; int cnt; } Poly;

#define LINEAR_SLOP 0.005f
#define BAUMGARTE 0.2f
#define MAX_BIAS 1.2f
#define SLEEP_TICKS 30
#define SLEEP_LIN2 (0.05f * 0.05f)
#define SLEEP_ANG2 (0.05f * 0.05f)

static inline float cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

// ------------------------------------------------------------------ setup
static uint16_t *s_parent;    // island union-find

void phys_init(void *(*alloc)(int), int maxb, int maxf, int maxj, int maxc) {
  memset(&W, 0, sizeof(W));
  W.maxb = maxb; W.maxf = maxf; W.maxj = maxj; W.maxc = maxc;
  W.bodies = alloc(sizeof(Body) * maxb);
  W.fix = alloc(sizeof(Fixture) * maxf);
  W.joints = alloc(sizeof(Joint) * maxj);
  W.contacts = alloc(sizeof(Contact) * maxc);
  W.hashsize = 1; while (W.hashsize < maxc * 2) W.hashsize <<= 1;
  W.hash = alloc(2 * W.hashsize);
  if (W.hash) for (int i = 0; i < W.hashsize; i++) W.hash[i] = 0xFFFF;
  W.order = alloc(2 * maxf);
  s_parent = alloc(2 * maxb);
  if (!W.bodies || !s_parent) return;
  W.gx = 0; W.gy = 10.0f;
  W.iterations = 8;
  W.activeX0 = -1e9f; W.activeY0 = -1e9f; W.activeX1 = 1e9f; W.activeY1 = 1e9f;
  body_create(BT_STATIC, 0, 0, 0);   // ground body (index 0)
}

int body_create(int type, float x, float y, float a) {
  if (W.nb >= W.maxb) return -1;
  int i = W.nb++;
  Body *b = &W.bodies[i];
  memset(b, 0, sizeof(*b));
  b->type = type;
  b->cx = x; b->cy = y; b->a = a; b->c = cosf(a); b->s = sinf(a);
  b->firstFix = W.nf;
  b->flags = BF_AWAKE;
  return i;
}

void body_origin(const Body *b, float *x, float *y) {
  *x = b->cx - (b->c * b->lcx - b->s * b->lcy);
  *y = b->cy - (b->s * b->lcx + b->c * b->lcy);
}

void body_set_xf(int bi, float x, float y, float a) {
  Body *b = &W.bodies[bi];
  b->a = a; b->c = cosf(a); b->s = sinf(a);
  b->cx = x + (b->c * b->lcx - b->s * b->lcy);
  b->cy = y + (b->s * b->lcx + b->c * b->lcy);
}

void body_world_point(const Body *b, float lx, float ly, float *wx, float *wy) {
  float ox, oy; body_origin(b, &ox, &oy);
  *wx = ox + b->c * lx - b->s * ly;
  *wy = oy + b->s * lx + b->c * ly;
}

void body_local_point(const Body *b, float wx, float wy, float *lx, float *ly) {
  float ox, oy; body_origin(b, &ox, &oy);
  float dx = wx - ox, dy = wy - oy;
  *lx = b->c * dx + b->s * dy;
  *ly = -b->s * dx + b->c * dy;
}

void body_vel_at(const Body *b, float wx, float wy, float *vx, float *vy) {
  *vx = b->vx - b->w * (wy - b->cy);
  *vy = b->vy + b->w * (wx - b->cx);
}

static int fix_new(int bi) {
  if (W.nf >= W.maxf) return -1;
  Body *b = &W.bodies[bi];
  if (b->nFix == 0) b->firstFix = W.nf;
  else if (b->firstFix + b->nFix != W.nf) return -1;   // fixtures must be contiguous
  int f = W.nf++;
  b->nFix++;
  Fixture *fx = &W.fix[f];
  memset(fx, 0, sizeof(*fx));
  fx->body = bi;
  fx->cat = 1; fx->mask = 0xFFFF; fx->group = 0;
  fx->friction = 100; fx->restitution = 10;
  fx->rc = 16384; fx->rs = 0;
  return f;
}


int fix_poly(int bi, const VI *v, int n, float ox, float oy, float a, float density) {
  int f = fix_new(bi);
  if (f < 0) return -1;
  Fixture *fx = &W.fix[f];
  fx->type = FX_POLY; fx->n = n > 8 ? 8 : n; fx->v = v;
  fx->ox = (int16_t)lrintf(ox * 64); fx->oy = (int16_t)lrintf(oy * 64);
  fx->rc = (int16_t)lrintf(cosf(a) * 16384); fx->rs = (int16_t)lrintf(sinf(a) * 16384);
  fx->r = (int16_t)lrintf(density * 100);   // density stored temporarily in r for mass computation
  return f;
}

int fix_circle(int bi, float ox, float oy, float r, float density) {
  int f = fix_new(bi);
  if (f < 0) return -1;
  Fixture *fx = &W.fix[f];
  fx->type = FX_CIRCLE; fx->n = (uint8_t)(density * 10 > 255 ? 255 : density * 10);
  fx->ox = (int16_t)lrintf(ox * 64); fx->oy = (int16_t)lrintf(oy * 64);
  fx->r = (int16_t)lrintf(r * 64);
  return f;
}

void fix_filter(int f, uint16_t cat, uint16_t mask, int16_t group) {
  if (f < 0) return;
  W.fix[f].cat = cat; W.fix[f].mask = mask; W.fix[f].group = group;
}

// local (body frame, origin relative) vertex of fixture
static inline void fix_local_vert(const Fixture *f, int i, float *x, float *y) {
  float vx = f->v[i].x * VI_SCALE, vy = f->v[i].y * VI_SCALE;
  float rc = f->rc * (1.0f / 16384), rs = f->rs * (1.0f / 16384);
  *x = f->ox * VI_SCALE + rc * vx - rs * vy;
  *y = f->oy * VI_SCALE + rs * vx + rc * vy;
}

void body_finalize(int bi, float densOverride) {
  Body *b = &W.bodies[bi];
  float ox, oy; body_origin(b, &ox, &oy);
  if (b->type == BT_STATIC) {
    b->lcx = b->lcy = 0; b->invM = b->invI = 0; b->mass = 0;
    b->cx = ox; b->cy = oy;
    for (int k = 0; k < b->nFix; k++) {
      Fixture *f = &W.fix[b->firstFix + k];
      if (f->type == FX_POLY) f->r = 0;
    }
    return;
  }
  float mass = 0, I = 0, mx = 0, my = 0;
  for (int k = 0; k < b->nFix; k++) {
    Fixture *f = &W.fix[b->firstFix + k];
    if (f->type == FX_CIRCLE) {
      float dens = densOverride > 0 ? densOverride : f->n / 10.0f;
      float r = f->r * VI_SCALE;
      float m = dens * 3.14159265f * r * r;
      float cx = f->ox * VI_SCALE, cy = f->oy * VI_SCALE;
      mass += m; mx += m * cx; my += m * cy;
      I += m * (0.5f * r * r + cx * cx + cy * cy);
      f->n = 0;
    } else {
      float dens = densOverride > 0 ? densOverride : f->r / 100.0f;
      f->r = 0;
      // polygon mass (Box2D b2PolygonShape::ComputeMass, reference = first vertex)
      float px, py; fix_local_vert(f, 0, &px, &py);
      float area = 0, cx = 0, cy = 0, inertia = 0;
      for (int i = 1; i + 1 < f->n; i++) {
        float x1, y1, x2, y2;
        fix_local_vert(f, i, &x1, &y1);
        fix_local_vert(f, i + 1, &x2, &y2);
        float e1x = x1 - px, e1y = y1 - py, e2x = x2 - px, e2y = y2 - py;
        float D = cross2(e1x, e1y, e2x, e2y);
        float ta = 0.5f * D;
        area += ta;
        cx += ta * (e1x + e2x) / 3.0f;
        cy += ta * (e1y + e2y) / 3.0f;
        float intx2 = e1x * e1x + e2x * e1x + e2x * e2x;
        float inty2 = e1y * e1y + e2y * e1y + e2y * e2y;
        inertia += (0.25f / 3.0f * D) * (intx2 + inty2);
      }
      if (area < 0) { area = -area; cx = -cx; cy = -cy; inertia = -inertia; }
      if (area < 1e-6f) continue;
      cx /= area; cy /= area;
      float mcx = cx + px, mcy = cy + py;
      float pm = dens * area;
      inertia = dens * inertia + pm * (mcx * mcx + mcy * mcy - cx * cx - cy * cy);
      cx = mcx * area; cy = mcy * area;   // area weighted for accumulation below
      float m = dens * area;
      mass += m; mx += dens * cx; my += dens * cy;
      I += inertia;
    }
  }
  if (mass < 1e-4f) {
    mass = 1; I = 0.1f; mx = my = 0;
  }
  b->mass = mass;
  float lcx = mx / mass, lcy = my / mass;
  I -= mass * (lcx * lcx + lcy * lcy);
  if (I < 1e-5f) I = 1e-5f;
  b->invM = 1.0f / mass;
  b->invI = (b->flags & BF_FIXEDROT) ? 0 : 1.0f / I;
  b->lcx = lcx; b->lcy = lcy;
  b->cx = ox + b->c * lcx - b->s * lcy;
  b->cy = oy + b->s * lcx + b->c * lcy;
}

void body_set_static(int bi, bool st) {
  Body *b = &W.bodies[bi];
  float ox, oy; body_origin(b, &ox, &oy);
  if (st) {
    b->type = BT_STATIC; b->vx = b->vy = b->w = 0; b->invM = b->invI = 0;
  } else if (b->type == BT_STATIC) {
    b->type = BT_DYNAMIC;
    // recompute mass with default density 1
    for (int k = 0; k < b->nFix; k++) {
      Fixture *f = &W.fix[b->firstFix + k];
      if (f->type == FX_POLY) f->r = 100; else f->n = 10;
    }
    body_finalize(bi, 0);
    b->flags |= BF_AWAKE; b->sleep = 0;
  }
  (void)ox; (void)oy;
}

static void wake_joints(int bi);

void body_wake(int bi) {
  if (bi <= 0) return;
  Body *b = &W.bodies[bi];
  if (b->type != BT_DYNAMIC || (b->flags & BF_DEAD)) return;
  b->sleep = 0;
  if (!(b->flags & BF_AWAKE)) {
    b->flags |= BF_AWAKE;
    wake_joints(bi);
  }
}

static void wake_joints(int bi) {
  for (int j = 0; j < W.nj; j++) {
    Joint *jt = &W.joints[j];
    if (jt->flags & JF_DEAD) continue;
    if (jt->ba == bi) body_wake(jt->bb);
    else if (jt->bb == bi) body_wake(jt->ba);
  }
}

void body_apply_impulse(int bi, float ix, float iy, float px, float py) {
  Body *b = &W.bodies[bi];
  if (b->type != BT_DYNAMIC) return;
  body_wake(bi);
  b->vx += ix * b->invM; b->vy += iy * b->invM;
  b->w += b->invI * cross2(px - b->cx, py - b->cy, ix, iy);
}

void body_apply_force(int bi, float fx, float fy, float px, float py, float dt) {
  body_apply_impulse(bi, fx * dt, fy * dt, px, py);
}

void body_kill(int bi) {
  if (bi <= 0) return;
  Body *b = &W.bodies[bi];
  b->flags |= BF_DEAD; b->flags &= ~BF_AWAKE;
  for (int k = 0; k < b->nFix; k++) W.fix[b->firstFix + k].flags |= FXF_DEAD;
  for (int j = 0; j < W.nj; j++)
    if (W.joints[j].ba == bi || W.joints[j].bb == bi) W.joints[j].flags |= JF_DEAD;
}

// ------------------------------------------------------------------ joints
static Joint *joint_new(int ba, int bb, int type) {
  if (W.nj >= W.maxj) return 0;
  Joint *j = &W.joints[W.nj++];
  memset(j, 0, sizeof(*j));
  j->type = type; j->ba = ba < 0 ? 0 : ba; j->bb = bb < 0 ? 0 : bb;
  return j;
}

int joint_revolute(int ba, int bb, float wx, float wy, bool limit, float lo, float hi, bool motor, float speed, float maxT, bool collide) {
  Joint *j = joint_new(ba, bb, JT_REVOLUTE);
  if (!j) return -1;
  Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
  body_local_point(A, wx, wy, &j->lax, &j->lay);
  body_local_point(B, wx, wy, &j->lbx, &j->lby);
  j->ref = B->a - A->a;
  j->lo = lo; j->hi = hi; j->speed = speed; j->maxF = maxT;
  j->flags = (limit ? JF_LIMIT : 0) | (motor ? JF_MOTOR : 0) | (collide ? JF_COLLIDE : 0);
  return (int)(j - W.joints);
}

int joint_prismatic(int ba, int bb, float wx, float wy, float axx, float axy, bool limit, float lo, float hi, bool motor, float speed, float maxF, bool collide) {
  Joint *j = joint_new(ba, bb, JT_PRISMATIC);
  if (!j) return -1;
  Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
  body_local_point(A, wx, wy, &j->lax, &j->lay);
  body_local_point(B, wx, wy, &j->lbx, &j->lby);
  j->axx = A->c * axx + A->s * axy;
  j->axy = -A->s * axx + A->c * axy;
  j->ref = B->a - A->a;
  j->lo = lo; j->hi = hi; j->speed = speed; j->maxF = maxF;
  j->flags = (limit ? JF_LIMIT : 0) | (motor ? JF_MOTOR : 0) | (collide ? JF_COLLIDE : 0);
  return (int)(j - W.joints);
}

int joint_weld(int ba, int bb, float wx, float wy) {
  Joint *j = joint_new(ba, bb, JT_WELD);
  if (!j) return -1;
  Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
  body_local_point(A, wx, wy, &j->lax, &j->lay);
  body_local_point(B, wx, wy, &j->lbx, &j->lby);
  j->ref = B->a - A->a;
  return (int)(j - W.joints);
}

int joint_rope(int ba, int bb, float awx, float awy, float bwx, float bwy, float len) {
  Joint *j = joint_new(ba, bb, JT_ROPE);
  if (!j) return -1;
  Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
  body_local_point(A, awx, awy, &j->lax, &j->lay);
  body_local_point(B, bwx, bwy, &j->lbx, &j->lby);
  j->hi = len;
  j->flags = JF_COLLIDE;
  return (int)(j - W.joints);
}

void joint_kill(int j) {
  if (j < 0 || j >= W.nj) return;
  Joint *jt = &W.joints[j];
  if (jt->flags & JF_DEAD) return;
  jt->flags |= JF_DEAD;
  body_wake(jt->ba); body_wake(jt->bb);
}

// ------------------------------------------------------------------ filters
bool should_collide(uint16_t catA, uint16_t maskA, int16_t gA, uint16_t catB, uint16_t maskB, int16_t gB) {
  if (gA == gB && gA != 0) return gA > 0;
  return (maskA & catB) != 0 && (maskB & catA) != 0;
}

void static_filter(int coll, bool fixed, uint16_t *cat, uint16_t *mask, int16_t *group) {
  // mirrors LevelB2D::setShapeFilter of the original game (level version > 1.84)
  *mask = 0xFFFF; *group = 0;
  switch (coll) {
    case 2: *cat = fixed ? 24 : 8; *mask = fixed ? 56 : 8; break;
    case 3: *cat = 0; *mask = 0; break;
    case 4: *group = -321; *cat = fixed ? 24 : 8; break;
    case 5: *group = -322; *cat = 16; *mask = fixed ? 48 : 16; break;
    case 6: *cat = fixed ? 48 : 32; *mask = 48; break;
    case 7: *cat = 15; *mask = 0x0F00; break;
    default:
      if (fixed) { *group = -10; *cat = 24; } else { *cat = 8; }
      break;
  }
}

// ------------------------------------------------------------------ geometry
static void fixture_poly(const Fixture *f, const Body *b, Poly *p) {
  float ox, oy; body_origin(b, &ox, &oy);
  int n = f->n;
  for (int i = 0; i < n; i++) {
    float lx, ly; fix_local_vert(f, i, &lx, &ly);
    p->v[i].x = ox + b->c * lx - b->s * ly;
    p->v[i].y = oy + b->s * lx + b->c * ly;
  }
  p->cnt = n;
}

static void poly_normals(Poly *p) {
  int n = p->cnt;
  if (n == 2) {
    float ex = p->v[1].x - p->v[0].x, ey = p->v[1].y - p->v[0].y;
    float l = sqrtf(ex * ex + ey * ey); if (l < 1e-9f) l = 1e-9f;
    p->n[0].x = ey / l; p->n[0].y = -ex / l;
    p->n[1].x = -p->n[0].x; p->n[1].y = -p->n[0].y;
    return;
  }
  for (int i = 0; i < n; i++) {
    int j = i + 1 == n ? 0 : i + 1;
    float ex = p->v[j].x - p->v[i].x, ey = p->v[j].y - p->v[i].y;
    float l = sqrtf(ex * ex + ey * ey); if (l < 1e-9f) l = 1e-9f;
    p->n[i].x = ey / l; p->n[i].y = -ex / l;
  }
}

static void fixture_circle(const Fixture *f, const Body *b, float *x, float *y, float *r) {
  float ox, oy; body_origin(b, &ox, &oy);
  float lx = f->ox * VI_SCALE, ly = f->oy * VI_SCALE;
  *x = ox + b->c * lx - b->s * ly;
  *y = oy + b->s * lx + b->c * ly;
  *r = f->r * VI_SCALE;
}

void fixture_world_aabb(const Fixture *f, float *x0, float *y0, float *x1, float *y1) {
  const Body *b = &W.bodies[f->body];
  if (f->type == FX_CIRCLE) {
    float x, y, r; fixture_circle(f, b, &x, &y, &r);
    *x0 = x - r; *y0 = y - r; *x1 = x + r; *y1 = y + r;
  } else {
    Poly p; fixture_poly(f, b, &p);
    float a = p.v[0].x, bb = p.v[0].y, c = a, d = bb;
    for (int i = 1; i < p.cnt; i++) {
      if (p.v[i].x < a) a = p.v[i].x;
      if (p.v[i].x > c) c = p.v[i].x;
      if (p.v[i].y < bb) bb = p.v[i].y;
      if (p.v[i].y > d) d = p.v[i].y;
    }
    *x0 = a; *y0 = bb; *x1 = c; *y1 = d;
  }
}

bool fixture_test_point(const Fixture *f, float px, float py) {
  const Body *b = &W.bodies[f->body];
  if (f->type == FX_CIRCLE) {
    float x, y, r; fixture_circle(f, b, &x, &y, &r);
    return (px - x) * (px - x) + (py - y) * (py - y) <= r * r;
  }
  Poly p; fixture_poly(f, b, &p); poly_normals(&p);
  for (int i = 0; i < p.cnt; i++)
    if (p.n[i].x * (px - p.v[i].x) + p.n[i].y * (py - p.v[i].y) > 0) return false;
  return true;
}

static void update_aabb(Fixture *f) {
  float x0, y0, x1, y1;
  fixture_world_aabb(f, &x0, &y0, &x1, &y1);
  const float m = 0.05f;
  f->aabb[0] = (int16_t)clampf(floorf((x0 - m) * 64), -32768, 32767);
  f->aabb[1] = (int16_t)clampf(floorf((y0 - m) * 64), -32768, 32767);
  f->aabb[2] = (int16_t)clampf(ceilf((x1 + m) * 64), -32768, 32767);
  f->aabb[3] = (int16_t)clampf(ceilf((y1 + m) * 64), -32768, 32767);
}

// ------------------------------------------------------------------ narrowphase
typedef struct { float nx, ny; int n; V2 p[2]; float sep[2]; uint32_t id[2]; } Manifold;

static float max_separation(const Poly *a, const Poly *b, int *edge) {
  float best = -1e30f; int be = 0;
  for (int i = 0; i < a->cnt; i++) {
    float nx = a->n[i].x, ny = a->n[i].y;
    float vx = a->v[i].x, vy = a->v[i].y;
    float mn = 1e30f;
    for (int j = 0; j < b->cnt; j++) {
      float d = nx * (b->v[j].x - vx) + ny * (b->v[j].y - vy);
      if (d < mn) mn = d;
    }
    if (mn > best) { best = mn; be = i; }
  }
  *edge = be;
  return best;
}

typedef struct { V2 v; uint32_t id; } ClipV;

static int clip_segment(ClipV out[2], const ClipV in[2], float nx, float ny, float off, int vIndexA) {
  int num = 0;
  float d0 = nx * in[0].v.x + ny * in[0].v.y - off;
  float d1 = nx * in[1].v.x + ny * in[1].v.y - off;
  if (d0 <= 0) out[num++] = in[0];
  if (d1 <= 0) out[num++] = in[1];
  if (d0 * d1 < 0) {
    float t = d0 / (d0 - d1);
    out[num].v.x = in[0].v.x + t * (in[1].v.x - in[0].v.x);
    out[num].v.y = in[0].v.y + t * (in[1].v.y - in[0].v.y);
    out[num].id = (uint32_t)vIndexA << 8 | (in[0].id & 0xFFFF00FF) | 0x80000000u;
    num++;
  }
  return num;
}

static void collide_polys(const Poly *A, const Poly *B, Manifold *m) {
  m->n = 0;
  int ea, eb;
  float sa = max_separation(A, B, &ea);
  if (sa > 0) return;
  float sb = max_separation(B, A, &eb);
  if (sb > 0) return;
  const Poly *r, *in; int edge, flip;
  if (sb > 0.98f * sa + 0.001f) { r = B; in = A; edge = eb; flip = 1; }
  else { r = A; in = B; edge = ea; flip = 0; }
  float rnx = r->n[edge].x, rny = r->n[edge].y;
  // incident edge
  int ie = 0; float mind = 1e30f;
  for (int i = 0; i < in->cnt; i++) {
    float d = rnx * in->n[i].x + rny * in->n[i].y;
    if (d < mind) { mind = d; ie = i; }
  }
  int ie2 = ie + 1 == in->cnt ? 0 : ie + 1;
  ClipV inc[2];
  inc[0].v = in->v[ie]; inc[0].id = (uint32_t)(edge) | (uint32_t)ie << 16;
  inc[1].v = in->v[ie2]; inc[1].id = (uint32_t)(edge) | (uint32_t)ie2 << 16;
  int e2 = edge + 1 == r->cnt ? 0 : edge + 1;
  V2 v1 = r->v[edge], v2 = r->v[e2];
  float tx = v2.x - v1.x, ty = v2.y - v1.y;
  float tl = sqrtf(tx * tx + ty * ty); if (tl < 1e-9f) return;
  tx /= tl; ty /= tl;
  ClipV c1[2], c2[2];
  if (clip_segment(c1, inc, -tx, -ty, -(tx * v1.x + ty * v1.y), edge) < 2) return;
  if (clip_segment(c2, c1, tx, ty, tx * v2.x + ty * v2.y, e2) < 2) return;
  float front = rnx * v1.x + rny * v1.y;
  int n = 0;
  for (int i = 0; i < 2; i++) {
    float sep = rnx * c2[i].v.x + rny * c2[i].v.y - front;
    if (sep <= 0.01f) {
      m->p[n].x = c2[i].v.x - rnx * sep * 0.5f;
      m->p[n].y = c2[i].v.y - rny * sep * 0.5f;
      m->sep[n] = sep;
      m->id[n] = c2[i].id | (flip ? 0x40000000u : 0);
      n++;
    }
  }
  m->n = n;
  if (flip) { m->nx = -rnx; m->ny = -rny; } else { m->nx = rnx; m->ny = rny; }
}

// polygon A vs circle B
static void collide_poly_circle(const Poly *A, float cx, float cy, float r, Manifold *m) {
  m->n = 0;
  float sep = -1e30f; int ni = 0;
  for (int i = 0; i < A->cnt; i++) {
    float s = A->n[i].x * (cx - A->v[i].x) + A->n[i].y * (cy - A->v[i].y);
    if (s > r) return;
    if (s > sep) { sep = s; ni = i; }
  }
  int i2 = ni + 1 == A->cnt ? 0 : ni + 1;
  V2 v1 = A->v[ni], v2 = A->v[i2];
  float nx, ny, s;
  if (sep < 1e-6f) {
    nx = A->n[ni].x; ny = A->n[ni].y; s = sep - r;
  } else {
    float u1 = (cx - v1.x) * (v2.x - v1.x) + (cy - v1.y) * (v2.y - v1.y);
    float u2 = (cx - v2.x) * (v1.x - v2.x) + (cy - v2.y) * (v1.y - v2.y);
    if (u1 <= 0 || u2 <= 0) {
      V2 v = u1 <= 0 ? v1 : v2;
      float dx = cx - v.x, dy = cy - v.y;
      float d2 = dx * dx + dy * dy;
      if (d2 > r * r) return;
      float d = sqrtf(d2);
      if (d < 1e-6f) { nx = A->n[ni].x; ny = A->n[ni].y; } else { nx = dx / d; ny = dy / d; }
      s = d - r;
    } else {
      nx = A->n[ni].x; ny = A->n[ni].y; s = sep - r;
    }
  }
  m->nx = nx; m->ny = ny; m->n = 1;
  m->sep[0] = s;
  m->p[0].x = cx - nx * (r + s * 0.5f);
  m->p[0].y = cy - ny * (r + s * 0.5f);
  m->id[0] = 0;
}

static void collide_circles(float ax, float ay, float ar, float bx, float by, float br, Manifold *m) {
  m->n = 0;
  float dx = bx - ax, dy = by - ay;
  float d2 = dx * dx + dy * dy, rr = ar + br;
  if (d2 > rr * rr) return;
  float d = sqrtf(d2);
  float nx = 0, ny = -1;
  if (d > 1e-6f) { nx = dx / d; ny = dy / d; }
  float s = d - rr;
  m->nx = nx; m->ny = ny; m->n = 1;
  m->sep[0] = s;
  m->p[0].x = ax + nx * (ar + s * 0.5f);
  m->p[0].y = ay + ny * (ar + s * 0.5f);
  m->id[0] = 0;
}

static void static_poly(const StaticPrim *sp, Poly *p) {
  const VI *v = W.sw.verts + sp->vofs;
  int n = sp->n > 8 ? 8 : sp->n;
  for (int i = 0; i < n; i++) { p->v[i].x = v[i].x * VI_SCALE; p->v[i].y = v[i].y * VI_SCALE; }
  p->cnt = n;
  poly_normals(p);
}

// fa is dynamic fixture; fb either fixture or static prim (fb & 0x8000)
static void collide_pair(int fa, int fb, Manifold *m) {
  const Fixture *A = &W.fix[fa];
  const Body *BA = &W.bodies[A->body];
  Poly pa, pb;
  float acx = 0, acy = 0, ar = 0;
  if (A->type == FX_POLY) { fixture_poly(A, BA, &pa); poly_normals(&pa); }
  else fixture_circle(A, BA, &acx, &acy, &ar);
  if (fb & 0x8000) {
    const StaticPrim *sp = &W.sw.prims[fb & 0x7FFF];
    if (sp->kind == 1) {
      const VI *v = W.sw.verts + sp->vofs;
      float bx = v->x * VI_SCALE, by = v->y * VI_SCALE, br = (sp->aabb[2] - sp->aabb[0]) * (0.5f * VI_SCALE);
      if (A->type == FX_POLY) {
        collide_poly_circle(&pa, bx, by, br, m);
      } else collide_circles(acx, acy, ar, bx, by, br, m);
      return;
    }
    static_poly(sp, &pb);
    if (sp->kind == 2) {
      // one-sided terrain edge (like b2ChainShape): ignore shapes behind it
      float cx, cy;
      if (A->type == FX_POLY) {
        cx = 0; cy = 0;
        for (int i = 0; i < pa.cnt; i++) { cx += pa.v[i].x; cy += pa.v[i].y; }
        cx /= pa.cnt; cy /= pa.cnt;
      } else { cx = acx; cy = acy; }
      if (pb.n[0].x * (cx - pb.v[0].x) + pb.n[0].y * (cy - pb.v[0].y) < -0.02f) { m->n = 0; return; }
    }
    if (A->type == FX_POLY) collide_polys(&pa, &pb, m);
    else { collide_poly_circle(&pb, acx, acy, ar, m); m->nx = -m->nx; m->ny = -m->ny; }
    if (sp->kind == 2 && m->n > 0 && -(m->nx * pb.n[0].x + m->ny * pb.n[0].y) < 0.1f) m->n = 0;
    return;
  }
  const Fixture *B = &W.fix[fb];
  const Body *BB = &W.bodies[B->body];
  if (B->type == FX_POLY) { fixture_poly(B, BB, &pb); poly_normals(&pb); }
  if (A->type == FX_POLY && B->type == FX_POLY) collide_polys(&pa, &pb, m);
  else if (A->type == FX_POLY) {
    float bx, by, br; fixture_circle(B, BB, &bx, &by, &br);
    collide_poly_circle(&pa, bx, by, br, m);
  } else if (B->type == FX_POLY) {
    collide_poly_circle(&pb, acx, acy, ar, m);
    m->nx = -m->nx; m->ny = -m->ny;
  } else {
    float bx, by, br; fixture_circle(B, BB, &bx, &by, &br);
    collide_circles(acx, acy, ar, bx, by, br, m);
  }
}

// ------------------------------------------------------------------ contacts
static inline uint32_t pair_hash(int a, int b) { return ((uint32_t)a * 73856093u) ^ ((uint32_t)b * 19349663u); }

static int contact_find(int fa, int fb) {
  uint32_t h = pair_hash(fa, fb) & (W.hashsize - 1);
  int i = W.hash[h];
  while (i != 0xFFFF) {
    Contact *c = &W.contacts[i];
    if (c->fa == fa && c->fb == fb) return i;
    i = c->next;
  }
  return -1;
}

static int s_stepTag;

static bool joined(int ba, int bb) {
  for (int j = 0; j < W.nj; j++) {
    const Joint *jt = &W.joints[j];
    if (jt->flags & (JF_DEAD | JF_COLLIDE)) continue;
    if ((jt->ba == ba && jt->bb == bb) || (jt->ba == bb && jt->bb == ba)) return true;
  }
  return false;
}

// new contacts collected here then merged
static void add_or_update(int fa, int fb, const Manifold *m) {
  const Fixture *A = &W.fix[fa];
  int idx = contact_find(fa, fb);
  Contact *c;
  if (idx < 0) {
    if (W.nc >= W.maxc) return;
    idx = W.nc++;
    c = &W.contacts[idx];
    memset(c, 0, sizeof(*c));
    c->fa = fa; c->fb = fb;
    uint32_t h = pair_hash(fa, fb) & (W.hashsize - 1);
    c->next = W.hash[h]; W.hash[h] = idx;
    float fr2, re2;
    if (fb & 0x8000) { fr2 = 1.0f; re2 = 0.1f; }
    else { fr2 = W.fix[fb].friction / 100.0f; re2 = W.fix[fb].restitution / 100.0f; }
    float fr1 = A->friction / 100.0f, re1 = A->restitution / 100.0f;
    c->fric = sqrtf(fr1 * fr2);
    c->rest = re1 > re2 ? re1 : re2;
    c->n = 0;
  } else c = &W.contacts[idx];
  // match points for warm starting
  CPoint np[2];
  for (int i = 0; i < m->n; i++) {
    memset(&np[i], 0, sizeof(CPoint));
    np[i].id = m->id[i];
    np[i].sep = m->sep[i];
    for (int k = 0; k < c->n; k++)
      if (c->p[k].id == m->id[i]) { np[i].pn = c->p[k].pn; np[i].pt = c->p[k].pt; break; }
    np[i].rax = m->p[i].x; np[i].ray = m->p[i].y;   // world point temporarily
  }
  c->n = m->n;
  c->nx = m->nx; c->ny = m->ny;
  for (int i = 0; i < m->n; i++) c->p[i] = np[i];
  c->flags = (uint8_t)s_stepTag | 0x80;    // touching this step
  if ((A->flags & FXF_SENSOR) || (!(fb & 0x8000) && (W.fix[fb].flags & FXF_SENSOR))) c->flags |= 0x40;
}

static void process_pair(int fa, int fb) {
  Manifold m;
  collide_pair(fa, fb, &m);
  if (m.n > 0) add_or_update(fa, fb, &m);
}

// ------------------------------------------------------------------ broadphase
static int fix_active(const Fixture *f) {
  if (f->flags & FXF_DEAD) return 0;
  const Body *b = &W.bodies[f->body];
  if (b->flags & (BF_DEAD | BF_FROZEN)) return 0;
  return 1;
}

static void broadphase(void) {
  // rebuild order if fixture count changed
  if (W.norder != W.nf) {
    for (int i = 0; i < W.nf; i++) W.order[i] = i;
    W.norder = W.nf;
  }
  // insertion sort by aabb x0
  for (int i = 1; i < W.norder; i++) {
    uint16_t k = W.order[i];
    int16_t kx = W.fix[k].aabb[0];
    int j = i - 1;
    while (j >= 0 && W.fix[W.order[j]].aabb[0] > kx) { W.order[j + 1] = W.order[j]; j--; }
    W.order[j + 1] = k;
  }
  for (int ii = 0; ii < W.norder; ii++) {
    int fa = W.order[ii];
    const Fixture *A = &W.fix[fa];
    if (!fix_active(A)) continue;
    const Body *BA = &W.bodies[A->body];
    int aAwake = BA->type == BT_DYNAMIC && (BA->flags & BF_AWAKE);
    for (int jj = ii + 1; jj < W.norder; jj++) {
      int fb = W.order[jj];
      const Fixture *B = &W.fix[fb];
      if (B->aabb[0] > A->aabb[2]) break;
      if (B->aabb[1] > A->aabb[3] || B->aabb[3] < A->aabb[1]) continue;
      if (B->body == A->body || !fix_active(B)) continue;
      const Body *BB = &W.bodies[B->body];
      int bAwake = BB->type == BT_DYNAMIC && (BB->flags & BF_AWAKE);
      if (!aAwake && !bAwake) continue;
      if (BA->type != BT_DYNAMIC && BB->type != BT_DYNAMIC) continue;
      if (!should_collide(A->cat, A->mask, A->group, B->cat, B->mask, B->group)) continue;
      if (joined(A->body, B->body)) continue;
      // dynamic one first
      if (BA->type == BT_DYNAMIC) process_pair(fa, fb); else process_pair(fb, fa);
    }
    // static world
    if (aAwake && W.sw.ncol > 0 && !(A->flags & FXF_NOSTATIC)) {
      float qx0 = A->aabb[0] * VI_SCALE, qx1 = A->aabb[2] * VI_SCALE;
      int c0 = (int)floorf((qx0 - W.sw.x0) / W.sw.colw);
      int c1 = (int)floorf((qx1 - W.sw.x0) / W.sw.colw);
      if (c0 < 0) c0 = 0;
      if (c1 >= W.sw.ncol) c1 = W.sw.ncol - 1;
      for (int c = c0; c <= c1; c++) {
        uint32_t start = W.sw.cols[c * 2]; uint32_t cnt = W.sw.cols[c * 2 + 1] & 0xFFFF;
        for (uint32_t k = 0; k < cnt; k++) {
          int pi = W.sw.colidx[start + k];
          const StaticPrim *sp = &W.sw.prims[pi];
          if (sp->aabb[0] > A->aabb[2] || sp->aabb[2] < A->aabb[0] || sp->aabb[1] > A->aabb[3] || sp->aabb[3] < A->aabb[1]) continue;
          int pc0 = (int)floorf((sp->aabb[0] * VI_SCALE - W.sw.x0) / W.sw.colw);
          if (pc0 < c0) pc0 = c0;
          if (pc0 != c) continue;   // handled in its first shared column
          uint16_t cat, mask; int16_t grp;
          static_filter(sp->coll, true, &cat, &mask, &grp);
          if (!should_collide(A->cat, A->mask, A->group, cat, mask, grp)) continue;
          process_pair(fa, 0x8000 | pi);
        }
      }
    }
  }
}

// ------------------------------------------------------------------ solver
static void contacts_prestep(float inv_dt) {
  for (int i = 0; i < W.nc; i++) {
    Contact *c = &W.contacts[i];
    if (c->flags & 0x40) continue;   // sensor
    Body *A = &W.bodies[W.fix[c->fa].body];
    Body *B = (c->fb & 0x8000) ? &W.bodies[0] : &W.bodies[W.fix[c->fb].body];
    float nx = c->nx, ny = c->ny, tx = -ny, ty = nx;
    for (int k = 0; k < c->n; k++) {
      CPoint *p = &c->p[k];
      float px = p->rax, py = p->ray;
      p->rax = px - A->cx; p->ray = py - A->cy;
      p->rbx = px - B->cx; p->rby = py - B->cy;
      float rna = cross2(p->rax, p->ray, nx, ny), rnb = cross2(p->rbx, p->rby, nx, ny);
      float kn = A->invM + B->invM + A->invI * rna * rna + B->invI * rnb * rnb;
      p->mn = kn > 0 ? 1.0f / kn : 0;
      float rta = cross2(p->rax, p->ray, tx, ty), rtb = cross2(p->rbx, p->rby, tx, ty);
      float kt = A->invM + B->invM + A->invI * rta * rta + B->invI * rtb * rtb;
      p->mt = kt > 0 ? 1.0f / kt : 0;
      float bias = -BAUMGARTE * inv_dt * (p->sep + LINEAR_SLOP);
      if (bias < 0) bias = 0;
      if (bias > MAX_BIAS) bias = MAX_BIAS;
      // restitution
      float dvx = B->vx - B->w * p->rby - A->vx + A->w * p->ray;
      float dvy = B->vy + B->w * p->rbx - A->vy - A->w * p->rax;
      float vn = dvx * nx + dvy * ny;
      if (vn < -1.0f) { float rb = -c->rest * vn; if (rb > bias) bias = rb; }
      p->bias = bias;
      // warm start
      float Px = p->pn * nx + p->pt * tx, Py = p->pn * ny + p->pt * ty;
      A->vx -= A->invM * Px; A->vy -= A->invM * Py; A->w -= A->invI * cross2(p->rax, p->ray, Px, Py);
      B->vx += B->invM * Px; B->vy += B->invM * Py; B->w += B->invI * cross2(p->rbx, p->rby, Px, Py);
    }
  }
}

static void contacts_solve(void) {
  for (int i = 0; i < W.nc; i++) {
    Contact *c = &W.contacts[i];
    if (c->flags & 0x40) continue;
    Body *A = &W.bodies[W.fix[c->fa].body];
    Body *B = (c->fb & 0x8000) ? &W.bodies[0] : &W.bodies[W.fix[c->fb].body];
    float nx = c->nx, ny = c->ny, tx = -ny, ty = nx;
    for (int k = 0; k < c->n; k++) {
      CPoint *p = &c->p[k];
      float dvx = B->vx - B->w * p->rby - A->vx + A->w * p->ray;
      float dvy = B->vy + B->w * p->rbx - A->vy - A->w * p->rax;
      // normal
      float vn = dvx * nx + dvy * ny;
      float dPn = p->mn * (-vn + p->bias);
      float pn0 = p->pn;
      p->pn = pn0 + dPn > 0 ? pn0 + dPn : 0;
      dPn = p->pn - pn0;
      float Px = dPn * nx, Py = dPn * ny;
      A->vx -= A->invM * Px; A->vy -= A->invM * Py; A->w -= A->invI * cross2(p->rax, p->ray, Px, Py);
      B->vx += B->invM * Px; B->vy += B->invM * Py; B->w += B->invI * cross2(p->rbx, p->rby, Px, Py);
      // friction
      dvx = B->vx - B->w * p->rby - A->vx + A->w * p->ray;
      dvy = B->vy + B->w * p->rbx - A->vy - A->w * p->rax;
      float vt = dvx * tx + dvy * ty;
      float dPt = -p->mt * vt;
      float maxPt = c->fric * p->pn;
      float pt0 = p->pt;
      p->pt = clampf(pt0 + dPt, -maxPt, maxPt);
      dPt = p->pt - pt0;
      Px = dPt * tx; Py = dPt * ty;
      A->vx -= A->invM * Px; A->vy -= A->invM * Py; A->w -= A->invI * cross2(p->rax, p->ray, Px, Py);
      B->vx += B->invM * Px; B->vy += B->invM * Py; B->w += B->invI * cross2(p->rbx, p->rby, Px, Py);
    }
  }
}

static void joints_prestep(float inv_dt) {
  for (int i = 0; i < W.nj; i++) {
    Joint *j = &W.joints[i];
    if (j->flags & JF_DEAD) continue;
    Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
    if (((A->flags | B->flags) & (BF_DEAD))) { j->flags |= JF_DEAD; continue; }
    if (!((A->flags & BF_AWAKE) && A->type == BT_DYNAMIC) && !((B->flags & BF_AWAKE) && B->type == BT_DYNAMIC)) { j->force = 0; continue; }
    if ((A->flags | B->flags) & BF_FROZEN) continue;
    float lax = j->lax - A->lcx, lay = j->lay - A->lcy;
    float lbx = j->lbx - B->lcx, lby = j->lby - B->lcy;
    j->rax = A->c * lax - A->s * lay; j->ray = A->s * lax + A->c * lay;
    j->rbx = B->c * lbx - B->s * lby; j->rby = B->s * lbx + B->c * lby;
    float mA = A->invM, mB = B->invM, iA = A->invI, iB = B->invI;
    if (j->type == JT_REVOLUTE || j->type == JT_WELD) {
      j->k11 = mA + mB + iA * j->ray * j->ray + iB * j->rby * j->rby;
      j->k12 = -iA * j->rax * j->ray - iB * j->rbx * j->rby;
      j->k22 = mA + mB + iA * j->rax * j->rax + iB * j->rbx * j->rbx;
      j->km = iA + iB > 0 ? 1.0f / (iA + iB) : 0;
      // positional error for Baumgarte
      float ex = (B->cx + j->rbx) - (A->cx + j->rax);
      float ey = (B->cy + j->rby) - (A->cy + j->ray);
      j->wx = -BAUMGARTE * inv_dt * ex; j->wy = -BAUMGARTE * inv_dt * ey;
      float ang = B->a - A->a - j->ref;
      j->kl = 0;
      if (j->type == JT_WELD) j->kl = -BAUMGARTE * inv_dt * ang;
      else if (j->flags & JF_LIMIT) {
        if (j->hi - j->lo < 0.01f) j->kl = -BAUMGARTE * inv_dt * (ang - j->lo), j->flags |= 0x40, j->flags &= ~0x30;
        else if (ang <= j->lo) { if (!(j->flags & 0x10)) j->il = 0; j->flags = (j->flags & ~0x60) | 0x10; j->kl = -BAUMGARTE * inv_dt * (ang - j->lo); }
        else if (ang >= j->hi) { if (!(j->flags & 0x20)) j->il = 0; j->flags = (j->flags & ~0x50) | 0x20; j->kl = -BAUMGARTE * inv_dt * (ang - j->hi); }
        else { j->flags &= ~0x70; j->il = 0; }
      } else j->il = 0;
      if (!(j->flags & JF_MOTOR)) j->im = 0;
      float Px = j->ix, Py = j->iy, L = j->im + j->il;
      A->vx -= mA * Px; A->vy -= mA * Py; A->w -= iA * (cross2(j->rax, j->ray, Px, Py) + L);
      B->vx += mB * Px; B->vy += mB * Py; B->w += iB * (cross2(j->rbx, j->rby, Px, Py) + L);
    } else if (j->type == JT_PRISMATIC) {
      float dx = (B->cx + j->rbx) - (A->cx + j->rax);
      float dy = (B->cy + j->rby) - (A->cy + j->ray);
      float ax = A->c * j->axx - A->s * j->axy, ay = A->s * j->axx + A->c * j->axy;
      float px = -ay, py = ax;
      float s1 = cross2(dx + j->rax, dy + j->ray, px, py), s2 = cross2(j->rbx, j->rby, px, py);
      float a1 = cross2(dx + j->rax, dy + j->ray, ax, ay), a2 = cross2(j->rbx, j->rby, ax, ay);
      float kp = mA + mB + iA * s1 * s1 + iB * s2 * s2;
      float ka = mA + mB + iA * a1 * a1 + iB * a2 * a2;
      float tr = ax * dx + ay * dy;
      // packed solver temps: k11 perp mass, k22 axial mass, k12=s1 kl=s2 rax=a1 ray=a2 rbx=translation rby=perp error
      j->k11 = kp > 0 ? 1.0f / kp : 0;
      j->k22 = ka > 0 ? 1.0f / ka : 0;
      j->km = iA + iB > 0 ? 1.0f / (iA + iB) : 0;
      j->k12 = s1; j->kl = s2; j->rax = a1; j->ray = a2; j->rbx = tr;
      j->rby = px * dx + py * dy;
      j->wx = ax; j->wy = ay;
      if (j->flags & JF_LIMIT) {
        if (tr <= j->lo) { if (!(j->flags & 0x10)) j->il = 0; j->flags = (j->flags & ~0x60) | 0x10; }
        else if (tr >= j->hi) { if (!(j->flags & 0x20)) j->il = 0; j->flags = (j->flags & ~0x50) | 0x20; }
        else { j->flags &= ~0x70; j->il = 0; }
      } else j->il = 0;
      if (!(j->flags & JF_MOTOR)) j->im = 0;
      // warm start: ix = perp impulse, iy = angular impulse, im = motor, il = limit
      float Pp = j->ix, Pa = j->iy, Pax = j->im + j->il;
      float Px = Pp * px + Pax * ax, Py = Pp * py + Pax * ay;
      float LA = Pp * s1 + Pax * a1 + Pa, LB = Pp * s2 + Pax * a2 + Pa;
      A->vx -= mA * Px; A->vy -= mA * Py; A->w -= iA * LA;
      B->vx += mB * Px; B->vy += mB * Py; B->w += iB * LB;
    } else if (j->type == JT_ROPE) {
      float dx = (B->cx + j->rbx) - (A->cx + j->rax);
      float dy = (B->cy + j->rby) - (A->cy + j->ray);
      float len = sqrtf(dx * dx + dy * dy);
      if (len > 1e-6f) { j->wx = dx / len; j->wy = dy / len; } else { j->wx = 0; j->wy = 1; }
      float C = len - j->hi;
      if (C <= 0) { j->im = 0; j->flags &= ~0x10; continue; }
      j->flags |= 0x10;
      float cra = cross2(j->rax, j->ray, j->wx, j->wy), crb = cross2(j->rbx, j->rby, j->wx, j->wy);
      float k = mA + mB + iA * cra * cra + iB * crb * crb;
      j->km = k > 0 ? 1.0f / k : 0;
      j->kl = -BAUMGARTE * inv_dt * C;
      float Px = j->im * j->wx, Py = j->im * j->wy;
      A->vx -= mA * Px; A->vy -= mA * Py; A->w -= iA * cross2(j->rax, j->ray, Px, Py);
      B->vx += mB * Px; B->vy += mB * Py; B->w += iB * cross2(j->rbx, j->rby, Px, Py);
    }
  }
}

static void joints_solve(float dt, float inv_dt) {
  for (int i = 0; i < W.nj; i++) {
    Joint *j = &W.joints[i];
    if (j->flags & JF_DEAD) continue;
    Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
    if (!((A->flags & BF_AWAKE) && A->type == BT_DYNAMIC) && !((B->flags & BF_AWAKE) && B->type == BT_DYNAMIC)) continue;
    if ((A->flags | B->flags) & BF_FROZEN) continue;
    float mA = A->invM, mB = B->invM, iA = A->invI, iB = B->invI;
    if (j->type == JT_REVOLUTE || j->type == JT_WELD) {
      if (j->type == JT_REVOLUTE && (j->flags & JF_MOTOR) && j->km > 0) {
        float Cdot = B->w - A->w - j->speed;
        float imp = -j->km * Cdot;
        float old = j->im, maxI = j->maxF * dt;
        j->im = clampf(old + imp, -maxI, maxI);
        imp = j->im - old;
        A->w -= iA * imp; B->w += iB * imp;
      }
      if (j->km > 0) {
        if (j->type == JT_WELD || (j->flags & 0x40)) {
          float Cdot = B->w - A->w;
          float imp = -j->km * (Cdot - j->kl);
          j->il += imp;
          A->w -= iA * imp; B->w += iB * imp;
        } else if (j->flags & 0x30) {
          float Cdot = B->w - A->w;
          float imp = -j->km * (Cdot - j->kl);
          float old = j->il;
          if (j->flags & 0x10) j->il = old + imp > 0 ? old + imp : 0;
          else j->il = old + imp < 0 ? old + imp : 0;
          imp = j->il - old;
          A->w -= iA * imp; B->w += iB * imp;
        }
      }
      float cvx = B->vx - B->w * j->rby - A->vx + A->w * j->ray;
      float cvy = B->vy + B->w * j->rbx - A->vy - A->w * j->rax;
      float rx = -cvx + j->wx, ry = -cvy + j->wy;
      float det = j->k11 * j->k22 - j->k12 * j->k12;
      if (det != 0) det = 1.0f / det;
      float Px = det * (j->k22 * rx - j->k12 * ry);
      float Py = det * (j->k11 * ry - j->k12 * rx);
      j->ix += Px; j->iy += Py;
      A->vx -= mA * Px; A->vy -= mA * Py; A->w -= iA * cross2(j->rax, j->ray, Px, Py);
      B->vx += mB * Px; B->vy += mB * Py; B->w += iB * cross2(j->rbx, j->rby, Px, Py);
    } else if (j->type == JT_PRISMATIC) {
      float ax = j->wx, ay = j->wy, px = -ay, py = ax;
      float a1 = j->rax, a2 = j->ray, s1 = j->k12, s2 = j->kl;
      if (j->flags & JF_MOTOR) {
        float Cdot = ax * (B->vx - A->vx) + ay * (B->vy - A->vy) + a2 * B->w - a1 * A->w;
        float imp = j->k22 * (j->speed - Cdot);
        float old = j->im, maxI = j->maxF * dt;
        j->im = clampf(old + imp, -maxI, maxI);
        imp = j->im - old;
        A->vx -= mA * imp * ax; A->vy -= mA * imp * ay; A->w -= iA * imp * a1;
        B->vx += mB * imp * ax; B->vy += mB * imp * ay; B->w += iB * imp * a2;
      }
      if (j->flags & 0x30) {
        float C = (j->flags & 0x10) ? j->rbx - j->lo : j->rbx - j->hi;
        float Cdot = ax * (B->vx - A->vx) + ay * (B->vy - A->vy) + a2 * B->w - a1 * A->w;
        float imp = -j->k22 * (Cdot + BAUMGARTE * inv_dt * C);
        float old = j->il;
        if (j->flags & 0x10) j->il = old + imp > 0 ? old + imp : 0;
        else j->il = old + imp < 0 ? old + imp : 0;
        imp = j->il - old;
        A->vx -= mA * imp * ax; A->vy -= mA * imp * ay; A->w -= iA * imp * a1;
        B->vx += mB * imp * ax; B->vy += mB * imp * ay; B->w += iB * imp * a2;
      }
      if (j->km > 0) {
        float Cdot = B->w - A->w;
        float C = B->a - A->a - j->ref;
        float imp = -j->km * (Cdot + BAUMGARTE * inv_dt * C);
        j->iy += imp;
        A->w -= iA * imp; B->w += iB * imp;
      }
      {
        float Cdot = px * (B->vx - A->vx) + py * (B->vy - A->vy) + s2 * B->w - s1 * A->w;
        float imp = -j->k11 * (Cdot + BAUMGARTE * inv_dt * j->rby);
        j->ix += imp;
        A->vx -= mA * imp * px; A->vy -= mA * imp * py; A->w -= iA * imp * s1;
        B->vx += mB * imp * px; B->vy += mB * imp * py; B->w += iB * imp * s2;
      }
    } else if (j->type == JT_ROPE) {
      if (!(j->flags & 0x10)) continue;
      float cvx = B->vx - B->w * j->rby - A->vx + A->w * j->ray;
      float cvy = B->vy + B->w * j->rbx - A->vy - A->w * j->rax;
      float Cdot = cvx * j->wx + cvy * j->wy;
      float imp = -j->km * (Cdot - j->kl);
      float old = j->im;
      j->im = old + imp < 0 ? old + imp : 0;
      imp = j->im - old;
      float Px = imp * j->wx, Py = imp * j->wy;
      A->vx -= mA * Px; A->vy -= mA * Py; A->w -= iA * cross2(j->rax, j->ray, Px, Py);
      B->vx += mB * Px; B->vy += mB * Py; B->w += iB * cross2(j->rbx, j->rby, Px, Py);
    }
  }
}

// prismatic perpendicular position drift correction (applied to positions directly)
static void joints_fix_positions(void) {
  for (int i = 0; i < W.nj; i++) {
    Joint *j = &W.joints[i];
    if (j->flags & JF_DEAD) continue;
    Body *A = &W.bodies[j->ba], *B = &W.bodies[j->bb];
    if ((A->flags | B->flags) & BF_FROZEN) continue;
    if (!((A->flags & BF_AWAKE) && A->type == BT_DYNAMIC) && !((B->flags & BF_AWAKE) && B->type == BT_DYNAMIC)) continue;
    float mA = A->invM, mB = B->invM;
    if (mA + mB <= 0) continue;
    if (j->type == JT_REVOLUTE || j->type == JT_WELD) {
      // soft projection of the anchor error (NGS-lite, linear only)
      float lax = j->lax - A->lcx, lay = j->lay - A->lcy;
      float lbx = j->lbx - B->lcx, lby = j->lby - B->lcy;
      float rax = A->c * lax - A->s * lay, ray = A->s * lax + A->c * lay;
      float rbx = B->c * lbx - B->s * lby, rby = B->s * lbx + B->c * lby;
      float ex = (B->cx + rbx) - (A->cx + rax);
      float ey = (B->cy + rby) - (A->cy + ray);
      float e2 = ex * ex + ey * ey;
      if (e2 < 0.0004f) continue;
      float k = 0.5f / (mA + mB);
      A->cx += mA * k * ex; A->cy += mA * k * ey;
      B->cx -= mB * k * ex; B->cy -= mB * k * ey;
    }
  }
}

// ------------------------------------------------------------------ islands / sleep
static int uf_find(int i) {
  while (s_parent[i] != i) { s_parent[i] = s_parent[s_parent[i]]; i = s_parent[i]; }
  return i;
}
static void uf_union(int a, int b) {
  a = uf_find(a); b = uf_find(b);
  if (a != b) s_parent[a] = b;
}

static void sleep_islands(void) {
  int nb = W.nb;
  for (int i = 0; i < nb; i++) s_parent[i] = i;
  for (int i = 0; i < W.nc; i++) {
    Contact *c = &W.contacts[i];
    if (c->fb & 0x8000) continue;
    if (c->flags & 0x40) continue;
    int a = W.fix[c->fa].body, b = W.fix[c->fb].body;
    if (W.bodies[a].type == BT_DYNAMIC && W.bodies[b].type == BT_DYNAMIC) uf_union(a, b);
  }
  for (int i = 0; i < W.nj; i++) {
    Joint *j = &W.joints[i];
    if (j->flags & JF_DEAD) continue;
    if (W.bodies[j->ba].type == BT_DYNAMIC && W.bodies[j->bb].type == BT_DYNAMIC) uf_union(j->ba, j->bb);
  }
  // min sleep per root stored in island field
  for (int i = 0; i < nb; i++) W.bodies[i].island = 0xFFFF;
  for (int i = 1; i < nb; i++) {
    Body *b = &W.bodies[i];
    if (b->type != BT_DYNAMIC || !(b->flags & BF_AWAKE) || (b->flags & (BF_DEAD | BF_FROZEN))) continue;
    int r = uf_find(i);
    uint16_t s = (b->flags & BF_KEEPAWAKE) ? 0 : b->sleep;
    if (s < W.bodies[r].island) W.bodies[r].island = s;
  }
  for (int i = 1; i < nb; i++) {
    Body *b = &W.bodies[i];
    if (b->type != BT_DYNAMIC || !(b->flags & BF_AWAKE) || (b->flags & (BF_DEAD | BF_FROZEN))) continue;
    int r = uf_find(i);
    if (W.bodies[r].island != 0xFFFF && W.bodies[r].island >= SLEEP_TICKS) {
      b->flags &= ~BF_AWAKE; b->vx = b->vy = b->w = 0;
    }
  }
}

// ------------------------------------------------------------------ step
void phys_step(float dt) {
  float inv_dt = 1.0f / dt;
  s_stepTag ^= 1;
  // freeze / unfreeze bodies by region
  for (int i = 1; i < W.nb; i++) {
    Body *b = &W.bodies[i];
    if (b->flags & BF_DEAD) continue;
    int out = b->cx < W.activeX0 || b->cx > W.activeX1 || b->cy < W.activeY0 || b->cy > W.activeY1;
    if (b->flags & BF_KEEPAWAKE) out = 0;
    if (out) b->flags |= BF_FROZEN; else b->flags &= ~BF_FROZEN;
  }
  // integrate velocities
  for (int i = 1; i < W.nb; i++) {
    Body *b = &W.bodies[i];
    if (b->type != BT_DYNAMIC || !(b->flags & BF_AWAKE) || (b->flags & (BF_DEAD | BF_FROZEN))) continue;
    if (!(b->flags & BF_NOGRAV)) { b->vx += dt * W.gx; b->vy += dt * W.gy; }
    b->vx *= 0.9995f; b->vy *= 0.9995f; b->w *= 0.998f;
    // speed cap (avoid tunneling explosions)
    float v2 = b->vx * b->vx + b->vy * b->vy;
    if (v2 > 60.0f * 60.0f) { float k = 60.0f / sqrtf(v2); b->vx *= k; b->vy *= k; }
    b->w = clampf(b->w, -60, 60);
  }
  // aabbs for active, non static-sleeping
  for (int i = 0; i < W.nf; i++) {
    Fixture *f = &W.fix[i];
    if (f->flags & FXF_DEAD) continue;
    Body *b = &W.bodies[f->body];
    if (b->flags & (BF_DEAD | BF_FROZEN)) continue;
    if ((b->flags & BF_AWAKE) || f->aabb[0] == f->aabb[2]) update_aabb(f);
  }
  // collision
  for (int i = 0; i < W.nc; i++) W.contacts[i].flags &= ~0x80;
  broadphase();
  // remove stale contacts & rebuild hash
  int k = 0;
  for (int i = 0; i < W.nc; i++) {
    Contact *c = &W.contacts[i];
    if (!(c->flags & 0x80)) continue;
    // a contact between an awake and a sleeping dynamic body wakes the sleeper
    if (!(c->fb & 0x8000) && !(c->flags & 0x40)) {
      Body *A = &W.bodies[W.fix[c->fa].body], *B = &W.bodies[W.fix[c->fb].body];
      if (A->type == BT_DYNAMIC && B->type == BT_DYNAMIC) {
        if ((A->flags & BF_AWAKE) && !(B->flags & BF_AWAKE)) body_wake(W.fix[c->fb].body);
        else if ((B->flags & BF_AWAKE) && !(A->flags & BF_AWAKE)) body_wake(W.fix[c->fa].body);
      }
    }
    if (k != i) W.contacts[k] = W.contacts[i];
    k++;
  }
  W.nc = k;
  for (int i = 0; i < W.hashsize; i++) W.hash[i] = 0xFFFF;
  for (int i = 0; i < W.nc; i++) {
    Contact *c = &W.contacts[i];
    uint32_t h = pair_hash(c->fa, c->fb) & (W.hashsize - 1);
    c->next = W.hash[h]; W.hash[h] = i;
  }
  // solve
  contacts_prestep(inv_dt);
  joints_prestep(inv_dt);
  for (int it = 0; it < W.iterations; it++) {
    joints_solve(dt, inv_dt);
    contacts_solve();
  }
  // impulses for game logic
  for (int i = 0; i < W.nc; i++) {
    Contact *c = &W.contacts[i];
    float s = 0;
    for (int q = 0; q < c->n; q++) s += c->p[q].pn;
    c->impulse = s;
  }
  for (int i = 0; i < W.nj; i++) {
    Joint *j = &W.joints[i];
    j->force = (j->type == JT_PRISMATIC) ? fabsf(j->ix) * inv_dt : sqrtf(j->ix * j->ix + j->iy * j->iy) * inv_dt;
  }
  // integrate positions
  for (int i = 1; i < W.nb; i++) {
    Body *b = &W.bodies[i];
    if (b->type != BT_DYNAMIC || !(b->flags & BF_AWAKE) || (b->flags & (BF_DEAD | BF_FROZEN))) continue;
    b->cx += dt * b->vx; b->cy += dt * b->vy;
    b->a += dt * b->w;
    b->c = cosf(b->a); b->s = sinf(b->a);
    float v2 = b->vx * b->vx + b->vy * b->vy;
    if (v2 < SLEEP_LIN2 && b->w * b->w < SLEEP_ANG2) { if (b->sleep < 60000) b->sleep++; }
    else b->sleep = 0;
  }
  joints_fix_positions();
  sleep_islands();
}

void query_aabb(float x0, float y0, float x1, float y1, QueryCB cb, void *ud) {
  int16_t a0 = (int16_t)clampf(x0 * 64, -32768, 32767), a1 = (int16_t)clampf(y0 * 64, -32768, 32767);
  int16_t a2 = (int16_t)clampf(x1 * 64, -32768, 32767), a3 = (int16_t)clampf(y1 * 64, -32768, 32767);
  for (int i = 0; i < W.nf; i++) {
    const Fixture *f = &W.fix[i];
    if (!fix_active(f)) continue;
    if (f->aabb[0] > a2 || f->aabb[2] < a0 || f->aabb[1] > a3 || f->aabb[3] < a1) continue;
    if (!cb(i, ud)) return;
  }
}
