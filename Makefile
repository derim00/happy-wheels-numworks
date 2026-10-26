# Happy Wheels for NumWorks
NWLINK = /home/user/tools/node_modules/.bin/nwlink
SRC = src/main.c src/gfx.c src/phys.c src/level.c src/character.c src/specials.c src/game.c
CFLAGS_COMMON = -std=c99 -Wall -Wno-unused-parameter -Isrc -Ibuild
DEV_CFLAGS = -mthumb -mfloat-abi=hard -mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -DPLATFORM_DEVICE=1 -I/home/user/tools/node_modules/nwlink/dist/eadk \
  -Os -ffunction-sections -fdata-sections -fsingle-precision-constant -ffast-math $(CFLAGS_COMMON)

all: build/HappyWheels.nwa

build/assets.bin build/assets.h build/chardefs.h build/sprnames.c: tools/build_assets.py
	python3 -I tools/build_assets.py "$(ASSETS)" build

build/assets.o: build/assets.bin
	cd build && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rodata.assets,alloc,load,readonly,data,contents assets.bin assets.o

build/icon.o: icon.png
	$(NWLINK) png-icon-o icon.png build/icon.o

DEV_OBJS = $(patsubst src/%.c,build/dev/%.o,$(SRC) src/platform_device.c) build/dev/sprnames.o

build/dev/%.o: src/%.c build/assets.h
	@mkdir -p build/dev
	arm-none-eabi-gcc $(DEV_CFLAGS) -c $< -o $@

build/dev/sprnames.o: build/sprnames.c
	@mkdir -p build/dev
	arm-none-eabi-gcc $(DEV_CFLAGS) -c $< -o $@

build/HappyWheels.nwa: $(DEV_OBJS) build/assets.o build/icon.o
	arm-none-eabi-gcc $(DEV_CFLAGS) -Wl,--relocatable -nostartfiles $^ -lm -o $@
	arm-none-eabi-strip --strip-debug $@

sim: build/sim

build/sim: $(SRC) src/platform_host.c build/sprnames.c build/assets.h
	gcc -O2 -g $(CFLAGS_COMMON) $(SRC) src/platform_host.c build/sprnames.c -lm -o build/sim

clean:
	rm -rf build/dev build/sim build/*.o build/*.nwa
