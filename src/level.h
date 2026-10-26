// Level data (flash resident, produced by tools/build_assets.py) and runtime state.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "phys.h"

typedef struct {
  char magic[4];
  float sx, sy;
  uint16_t bg, bgc;
  uint8_t chr, pad0; uint16_t pad1;
  uint16_t nTop, nBodyShapes, nGroups, nJoints, nSpecials, nTriggers, nTargets, nSDraw, nSColl, nCol, nVerts, pad2;
  float bx0, by0, bx1, by1, colw;
  uint32_t o_vp, o_remap, o_shp, o_grp, o_jnt, o_spc, o_trg, o_tgt, o_sd, o_sc, o_col, o_colidx, o_dcol, o_dcolidx;
} LvHeader;

enum { ST_RECT, ST_CIRC, ST_TRI, ST_POLY, ST_ART, ST_TERR };
enum { SF_FIXED = 1, SF_SLEEP = 2, SF_INGROUP = 4, SF_TARGET = 8, SF_OUTLINE = 16, SF_INTER = 32 };

typedef struct {
  uint8_t type, flags, coll, opac;
  uint16_t color, outline, group, nverts;
  uint32_t vofs;
  int16_t x, y, a, r;       // 1/64 m, angle*8192
  uint16_t dens, z;         // density*100
  uint8_t cut, pad; uint16_t pad2;
} LvShape;

typedef struct { float x, y, a; uint8_t flags, opac; uint16_t first, n, z; } LvGroup;   // flags: 1 fixed 2 sleep 4 immovable 8 fixedrot
typedef struct { uint8_t t, fl; uint16_t pad, b1, b2; float x, y, lo, hi, spd, tq, ax, pad2; } LvJoint;
typedef struct { uint8_t t, pad; uint16_t group; float x, y; float p[8]; uint16_t z, pad2; } LvSpecial;
typedef struct { float x, y, w, h, a; uint8_t by, type, rep, sd; float d, i; uint16_t first, n; } LvTrigger;
typedef struct { uint8_t k, a; uint16_t i; float p[3]; } LvTarget;
typedef struct { uint8_t kind, opac, outline, cut; uint16_t color, oc, z, n; uint32_t vofs; int16_t aabb[4]; int16_t x, y, a, r; } LvSDraw;

// body tags
enum { TAG_NONE, TAG_SHAPE, TAG_GROUP, TAG_SPECIAL, TAG_CHAR, TAG_VEH, TAG_DEBRIS };

typedef struct {
  uint16_t first, n;      // shape records drawn with this body
  uint8_t opac, kind;
} BodyDraw;

#define MAX_SPECIALS 200
typedef struct {
  uint8_t type, state, flags, pad;
  uint16_t rec;           // index into LvSpecial
  int16_t body, body2, body3, joint;
  float t, v0, v1;        // timers / misc
  int16_t z;
  int16_t aux;
} Special;

typedef struct {
  uint8_t enabled, fired, inside, pad;
  uint16_t count;
  float timer, delay;
  float rep;
} TrigRT;

typedef struct {
  const LvHeader *h;
  const uint8_t *base;
  const VI *verts;
  const LvShape *shapes;
  const uint16_t *remap;
  const LvGroup *groups;
  const LvJoint *joints;
  const LvSpecial *specials;
  const LvTrigger *triggers;
  const LvTarget *targets;
  const LvSDraw *sdraw;
  const uint32_t *dcol;
  const uint16_t *dcolidx;
  // runtime maps
  int16_t *shapeBody;     // body shape index -> body (or -1)
  int16_t *groupBody;     // group -> body
  int16_t *jointMap;      // level joint -> physics joint
  BodyDraw *bdraw;        // per physics body
  uint8_t *shapeAlpha;    // per body shape current opacity (0..100)
  Special *sp; int nsp;
  TrigRT *trig;
  int level;
  float time;
  bool complete;
  int bodyStart;          // first body created by the level
} Level;

extern Level L;

bool level_load(int idx, void *(*alloc)(int));
void level_draw(void);
void level_update(float dt);
int level_ref_body(uint16_t ref);
void level_trigger_activate(int ti);
uint16_t level_bg_color(void);
void level_draw_background(void);
