// Compact 2D rigid body physics (Box2D-like sequential impulses) for NumWorks.
// World units: meters, y axis pointing DOWN (level XML convention).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

typedef struct { int16_t x, y; } VI;          // 1/64 m fixed point
#define VI_SCALE (1.0f / 64.0f)

enum { FX_CIRCLE = 0, FX_POLY = 1 };
enum { BT_STATIC = 0, BT_DYNAMIC = 1 };
enum { BF_AWAKE = 1, BF_FIXEDROT = 2, BF_FROZEN = 4, BF_DEAD = 8, BF_NOGRAV = 16, BF_KEEPAWAKE = 32, BF_BULLET = 64 };
enum { FXF_SENSOR = 1, FXF_DEAD = 2, FXF_NOSTATIC = 4 };
enum { JT_REVOLUTE = 0, JT_PRISMATIC = 1, JT_WELD = 2, JT_ROPE = 3 };
enum { JF_LIMIT = 1, JF_MOTOR = 2, JF_COLLIDE = 4, JF_DEAD = 8 };

typedef struct {
  float cx, cy;          // world center of mass
  float a, c, s;         // angle + cache
  float lcx, lcy;        // local center of mass (relative to origin)
  float vx, vy, w;
  float invM, invI;
  float mass;
  uint16_t firstFix;
  uint8_t nFix, type;
  uint8_t flags, tag;
  uint16_t user;
  uint16_t sleep;        // ticks below sleep tolerance
  uint16_t island;
} Body;

typedef struct {
  uint8_t type, n, flags, tag;
  uint16_t body, user;
  uint16_t cat, mask;
  int16_t group;
  uint8_t friction, restitution;   // x100
  const VI *v;                     // local verts (fixture frame)
  int16_t ox, oy;                  // fixture frame in body (origin relative), 1/64 m
  int16_t rc, rs;                  // fixture frame rotation, Q14
  int16_t r;                       // circle radius 1/64 m
  int16_t aabb[4];                 // 1/64 m world
} Fixture;

typedef struct {
  float rax, ray, rbx, rby;
  float pn, pt, mn, mt, bias, sep;
  uint32_t id;
} CPoint;

typedef struct {
  uint16_t fa, fb;                 // fb & 0x8000 => static prim index
  uint8_t n, flags;
  uint16_t next;
  float nx, ny;
  float fric, rest;
  float impulse;                   // total normal impulse of last step (for game events)
  CPoint p[2];
} Contact;

typedef struct {
  uint8_t type, flags;
  uint16_t ba, bb;
  float lax, lay, lbx, lby;        // local anchors (origin relative)
  float ref, lo, hi, speed, maxF;
  float axx, axy;                  // prismatic axis (local in A)
  float ix, iy, im, il;            // accumulated impulses (point x,y / motor / limit or angular)
  // solver temps
  float rax, ray, rbx, rby;
  float k11, k12, k22, km, kl;
  float wx, wy;                    // prismatic world axis
  float force;                     // last reaction force magnitude
  uint16_t tag, user;
} Joint;

// static world (flash) collision primitive
typedef struct {
  uint8_t kind, coll;              // 0 poly, 1 circle, 2 edge
  uint16_t n;
  uint32_t vofs;
  int16_t aabb[4];
} StaticPrim;

typedef struct {
  const StaticPrim *prims;
  const VI *verts;
  const uint16_t *colidx;
  const uint32_t *cols;            // pairs: (start, count) packed 8 bytes per column
  int ncol;
  float x0, colw;
  int nprims;
} StaticWorld;

typedef struct {
  Body *bodies; int nb, maxb;
  Fixture *fix; int nf, maxf;
  Joint *joints; int nj, maxj;
  Contact *contacts; int nc, maxc;
  uint16_t *hash; int hashsize;
  uint16_t *order; int norder;     // broadphase order of fixtures
  StaticWorld sw;
  float gx, gy;
  float activeX0, activeY0, activeX1, activeY1;   // simulation region
  int iterations;
} World;

extern World W;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

void phys_init(void *(*alloc)(int), int maxb, int maxf, int maxj, int maxc);
int body_create(int type, float x, float y, float a);
void body_set_xf(int b, float x, float y, float a);
void body_origin(const Body *b, float *x, float *y);
void body_finalize(int b, float densityOverride);   // compute mass from fixtures
int fix_poly(int b, const VI *v, int n, float ox, float oy, float a, float density);
int fix_circle(int b, float ox, float oy, float r, float density);
void fix_filter(int f, uint16_t cat, uint16_t mask, int16_t group);
void body_wake(int b);
void body_apply_impulse(int b, float ix, float iy, float px, float py);
void body_apply_force(int b, float fx, float fy, float px, float py, float dt);
void body_world_point(const Body *b, float lx, float ly, float *wx, float *wy);
void body_local_point(const Body *b, float wx, float wy, float *lx, float *ly);
void body_vel_at(const Body *b, float wx, float wy, float *vx, float *vy);
void body_set_static(int b, bool st);
void body_kill(int b);

int joint_revolute(int ba, int bb, float wx, float wy, bool limit, float lo, float hi, bool motor, float speed, float maxT, bool collide);
int joint_prismatic(int ba, int bb, float wx, float wy, float axx, float axy, bool limit, float lo, float hi, bool motor, float speed, float maxF, bool collide);
int joint_weld(int ba, int bb, float wx, float wy);
int joint_rope(int ba, int bb, float awx, float awy, float bwx, float bwy, float len);
void joint_kill(int j);

void phys_step(float dt);
bool should_collide(uint16_t catA, uint16_t maskA, int16_t gA, uint16_t catB, uint16_t maskB, int16_t gB);
void static_filter(int coll, bool fixed, uint16_t *cat, uint16_t *mask, int16_t *group);
void fixture_world_aabb(const Fixture *f, float *x0, float *y0, float *x1, float *y1);
bool fixture_test_point(const Fixture *f, float px, float py);
// query: callback for each fixture whose aabb overlaps
typedef bool (*QueryCB)(int fix, void *ud);
void query_aabb(float x0, float y0, float x1, float y1, QueryCB cb, void *ud);
void body_reset_contacts_flag(void);
