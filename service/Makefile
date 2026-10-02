# nuvio-ps5: resident Nuvio companion payload for jailbroken PS5 consoles.
#
#   make            build nuvio-ps5.elf (web app, tile assets and CA bundle embedded)
#   make deploy     send it to the console's elfldr (PS5_HOST, PS5_PORT)
#
# The web app: upstream NuvioTVSmart with web/nuvio-ps5.patch applied (npm run build).

SDK        ?= $(HOME)/ps5sdk/opt/ps5-payload-sdk
LLVM_CONFIG ?= /opt/homebrew/opt/llvm/bin/llvm-config
export LLVM_CONFIG

CC    := $(SDK)/bin/prospero-clang
STRIP := $(SDK)/bin/prospero-strip
HB    := $(SDK)/target/user/homebrew

WEB_DIST ?= ../web/NuvioTVSmart/dist
# Nuvio's public runtime config (nuvio.env.js) from an official Nuvio TV release.
WEB_ENV  ?= ../web/nuvio.env.js
# The Nuvio app (web UI host + Nuvio Player), built by ../app/scripts/build.sh.
APP_DIST ?= ../app/build/app/PPSA99176

PS5_HOST ?= ps5.local
PS5_PORT ?= 9021

ELF := nuvio-ps5.elf

CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude -I$(HB)/include \
           -ffunction-sections -fdata-sections
LDFLAGS := -Wl,--gc-sections
LIBS    := -L$(HB)/lib -lmicrohttpd -lcurl -lpsl \
           -lavformat -lavcodec -lswresample -lavutil -ldav1d -lxml2 -llzma -lx264 -liconv \
           -lssl -lcrypto -larchive \
           -llzma -lzstd -lbz2 -lz -ljansson -lm -lpthread \
           -lSceSystemService -lSceUserService -lSceAppInstUtil

SRCS := src/main.c src/http.c src/plugin_fetch.c src/tile.c src/www.c \
        src/fsutil.c src/log.c src/notify.c src/launch.c src/app_install.c src/player_api.c src/debug.c src/stream.c \
        src/hbldr/hbldr.c src/hbldr/elfldr.c src/hbldr/pt.c

GENERATED := build/www.tar.gz build/www.id build/icon0.png build/pic0.png build/nuvio-shell.elf \
             build/app.tar.gz build/app.id

all: $(ELF)

$(ELF): $(SRCS) include/nuvio.h include/assets.h assets/param.json assets/cacert.pem $(GENERATED)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)
	$(STRIP) $@
	@ls -lh $@
	@shasum -a 256 $@

build:
	mkdir -p build

# Full-screen launcher run as the foreground app (see shell/main.c). The SDK
# ships no stubs for the dialog libraries, so link against generated ones.
STUB_LIBS := build/stubs/libSceWebBrowserDialog.so build/stubs/libSceCommonDialog.so

build/stubs/%.so: shell/stubs/%.c | build
	mkdir -p build/stubs
	$(dir $(LLVM_CONFIG))clang --target=x86_64-unknown-freebsd -fPIC -c -o build/stubs/$*.o $<
	ld.lld -shared -soname $*.sprx -o $@ build/stubs/$*.o

build/nuvio-shell.elf: shell/main.c $(STUB_LIBS)
	$(CC) -O2 -Wall -o $@ shell/main.c -Lbuild/stubs -lSceWebBrowserDialog \
	  -lSceCommonDialog -lSceSysmodule -lSceUserService
	$(STRIP) $@

# PS5 tile icon: 512x512 opaque RGB PNG.
build/icon0.png: assets/icon-source.png | build
	ffmpeg -loglevel error -y -i $< -vf scale=512:512:flags=lanczos,format=rgb24 -frames:v 1 $@

# Home screen background shown while the tile is focused: 1920x1080 opaque.
build/pic0.png: assets/pic-source.png | build
	ffmpeg -loglevel error -y -f lavfi -i color=c=0x0d0e12:s=1920x1080 -i $< \
	  -filter_complex "[0][1]overlay=0:0,format=rgb24" -frames:v 1 $@

build/www.tar.gz: $(WEB_DIST)/index.html $(WEB_DIST)/app.bundle.js $(WEB_ENV) | build
	rm -rf build/www && mkdir -p build/www
	cp -R $(WEB_DIST)/. build/www/
	cp $(WEB_ENV) build/www/nuvio.env.js
	COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -czf $@ -C build/www .

build/app.tar.gz: $(APP_DIST)/eboot.bin $(APP_DIST)/sce_sys/param.json | build
	COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -czf $@ -C $(APP_DIST) .

build/app.id: build/app.tar.gz
	shasum -a 256 $< | cut -c1-12 > $@

build/www.id: build/www.tar.gz
	shasum -a 256 $< | cut -c1-12 > $@

deploy: $(ELF)
	nc -w 3 $(PS5_HOST) $(PS5_PORT) < $(ELF)

clean:
	rm -rf build $(ELF)

.PHONY: all deploy clean
