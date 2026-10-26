// Platform abstraction: NumWorks EADK on device, PNG dumping simulator on host.
#pragma once
#include <stdint.h>
#include <stdbool.h>

enum {
  K_LEFT = 0, K_UP = 1, K_DOWN = 2, K_RIGHT = 3, K_OK = 4, K_BACK = 5, K_HOME = 6,
  K_SHIFT = 12, K_ALPHA = 13, K_XNT = 14, K_VAR = 15, K_TOOLBOX = 16, K_BACKSPACE = 17,
  K_EXE = 52, K_ZERO = 48, K_ONE = 42, K_TWO = 43, K_THREE = 44, K_FOUR = 36, K_FIVE = 37, K_SIX = 38,
  K_SEVEN = 30, K_EIGHT = 31, K_NINE = 32, K_PLUS = 45, K_MINUS = 46
};

void plat_init(void);
void plat_push_strip(int y, const uint16_t *pixels);   // 320 x STRIP_H
uint64_t plat_keys(void);
uint32_t plat_millis(void);
void plat_sleep(uint32_t ms);
void *plat_arena(int *size);                             // big RAM block for level data
uint32_t plat_random(void);
bool plat_should_quit(void);
// persistent progress (stored in RAM only on device: NumWorks apps have no file API)
static inline bool key_down(uint64_t k, int key) { return (k >> key) & 1; }
