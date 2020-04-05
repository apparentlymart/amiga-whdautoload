
# This Makefile expects:
# - vbcc, vasm, and vlink binaries in $VBCC/bin
# - $VBCC/bin to be in the current $PATH
# - A vc configuration for "aos68k" in $VBCC/config
# - The vbcc m68k-amigaos target files in $VBCC/targets
# - The Amiga OS 3.9 NDK in $VBCC/ndk3.9 (though this project uses only OS 3.1 features)

CC=vc +aos68k
CFLAGS=-O4 -I$(VBCC)/targets/m68k-amigaos/include -I$(VBCC)/ndk3.9/Include/include_h
LDFLAGS=-L$(VBCC)/ndk3.9/Include/linker_libs -lamiga -lvc
C_FILES := $(shell find -maxdepth 1 -name "*.c")
H_FILES := $(shell find -maxdepth 1 -name "*.h")
O_FILES := $(patsubst %.c,%.o,$(C_FILES))

build/WHDAutoload: $(O_FILES)
	$(CC) -c99 $(LDFLAGS) $^ -final -o="$@"

%.o: %.c $(H_FILES)
	$(CC) "$<" -c99 -c -o="$@" $(CFLAGS)

clean:
	rm -rf $(shell find -name "*.o") build/* dist/*

dist: build/WHDAutoload
	mkdir -p dist/archive/WHDAutoload/source
	cp README dist/whdautoload.readme
	cp README dist/archive/WHDAutoload/README
	cp LICENSE dist/archive/WHDAutoload/LICENSE
	cp build/WHDAutoload dist/archive/WHDAutoload/WHDAutoload
	cp $(C_FILES) $(H_FILES) dist/archive/WHDAutoload/source
	fs-uae --chip_memory=4096 --hard_drive_0=archive-emu --hard_drive_0_priority=1 --hard_drive_1=dist --joystick_port_1=none --amiga_model=A1200 --floppy_drive_volume=0

.PHONY: clean dist
