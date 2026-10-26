#include "platform.h"
#include "gfx.h"
#include <eadk.h>

extern const uint8_t _binary_assets_bin_start[];
extern char _heap_start, _heap_end;

void plat_init(void) { g_assets = _binary_assets_bin_start; }

void plat_push_strip(int y, const uint16_t *pixels) {
  eadk_rect_t r = {0, (uint16_t)y, SCR_W, STRIP_H};
  eadk_display_push_rect(r, pixels);
}

uint64_t plat_keys(void) { return eadk_keyboard_scan(); }
uint32_t plat_millis(void) { return (uint32_t)eadk_timing_millis(); }
void plat_sleep(uint32_t ms) { eadk_timing_msleep(ms); }
uint32_t plat_random(void) { return eadk_random(); }
bool plat_should_quit(void) { return false; }

void *plat_arena(int *size) {
  // all RAM between the end of .bss and the end of the app RAM, minus a safety margin
  char *a = (char *)(((uintptr_t)&_heap_start + 7) & ~(uintptr_t)7);
  int s = (int)(&_heap_end - a) - 12 * 1024;
  *size = s;
  return a;
}

// newlib stubs (nothing uses them, keeps the linker quiet)
void *_sbrk(int incr) { (void)incr; return (void *)-1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }
void _exit(int status) { (void)status; while (1) {} }
int _write(int fd, const void *b, int n) { (void)fd; (void)b; return n; }
int _read(int fd, void *b, int n) { (void)fd; (void)b; (void)n; return 0; }
int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int o, int w) { (void)fd; (void)o; (void)w; return 0; }
int _fstat(int fd, void *st) { (void)fd; (void)st; return -1; }
int _isatty(int fd) { (void)fd; return 1; }
