#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "phys.h"

#define Z_LEVEL 16
#define Z_CHAR 60000
#define Z_PART 62000
#define Z_HUD 64000

// input (edge + level)
typedef struct { uint64_t down, pressed; } Input;
extern Input IN;
static inline bool in_down(int k) { return (IN.down >> k) & 1; }
static inline bool in_pressed(int k) { return (IN.pressed >> k) & 1; }

// sprites by name
int sprite_find(const char *name);

// specials
void special_create(int idx);
int special_body(int idx);
void specials_update(float dt);
void specials_draw(void);
void special_activate(int idx, int action, const float *p);
bool special_contact(int fixA, int fixB, float impulse);  // returns true if handled

// character
enum { P_CHEST, P_PELVIS, P_HEAD, P_UARM1, P_LARM1, P_UARM2, P_LARM2, P_ULEG1, P_LLEG1, P_ULEG2, P_LLEG2, NPARTS };
enum { J_NECK, J_SHO1, J_ELB1, J_SHO2, J_ELB2, J_WAIST, J_HIP1, J_KNEE1, J_HIP2, J_KNEE2, NRJ };
typedef struct {
  int16_t b[NPARTS];
  int16_t j[NRJ];
  float target[NRJ];
  int16_t spr[NPARTS];
  int16_t sprAlt[NPARTS];     // damaged look
  int16_t headChunk[4], chestChunk, brain;
  uint16_t broken;            // broken joints
  uint8_t headGone, impaled, kid, pad;
  float scale;
} Ragdoll;

typedef struct {
  int chr;                    // character id (1 wheelchair, 2 business, 3 dad, 4 shopper, 9 pogo)
  int veh;                    // vehicle type
  Ragdoll rd, kid;
  int16_t vb[8];              // vehicle bodies
  int16_t vj[8];              // vehicle joints
  int16_t att[8];             // attachment joints (character to vehicle)
  int16_t kidAtt[4];
  int natt, nkidAtt;
  bool dead, ejected, vehBroken;
  float deadTimer, jumpCool, jet, pogoCharge;
  float lean;                 // -1..1 current lean input
  float x, y;                 // focus position
  float vx, vy;
  int bleed;
} Player;
extern Player PL;

void player_create(int chr, float sx, float sy);
void player_update(float dt);
void player_draw(void);
void player_after_step(float dt);
bool player_owns_body(int b);
bool player_is_char_body(int b);
void player_impale(int b, int spikeBody, float wx, float wy);
void player_kill(void);

// particles
void particles_reset(void);
void particle_blood(float x, float y, float vx, float vy, int n);
void particle_spawn(int spr, float x, float y, float vx, float vy, float life, float spin);
void particles_update(float dt);
void particles_draw(void);

// game flow
void game_level_complete(void);
float frand(void);
uint32_t plat_frame_counter(void);
#include "platform.h"
