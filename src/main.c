#include "platform.h"
#include "gfx.h"
#include <stdlib.h>

void game_init(void);
bool game_frame(float dt);
void game_debug_start(int level);

#ifdef PLATFORM_DEVICE
const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "Happy Wheels";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;
#endif

int main(int argc, char *argv[]) {
  plat_init();
  game_init();
#ifndef PLATFORM_DEVICE
  if (argc > 1 && atoi(argv[1]) >= 0) game_debug_start(atoi(argv[1]));
#else
  (void)argc; (void)argv;
#endif
  uint32_t last = plat_millis();
  while (!plat_should_quit()) {
    uint32_t now = plat_millis();
    float dt = (now - last) / 1000.0f;
    // real time down to 20 fps (more physics sub-steps), slower than that the game slows down
    if (dt < 1.0f / 30) dt = 1.0f / 30;
    if (dt > 1.0f / 20) dt = 1.0f / 20;
    if (!game_frame(dt)) break;
    uint32_t spent = plat_millis() - now;
    if (spent < 33) plat_sleep(33 - spent);
    last = now;
#ifdef PLATFORM_DEVICE
    if (plat_keys() & (1ull << K_HOME)) break;
#endif
  }
  return 0;
}
