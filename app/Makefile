# Nuvio PS5
# Copyright (C) 2026 Husam Osman
# SPDX-License-Identifier: GPL-3.0-or-later
# app: object build only. scripts/build.sh drives this with the PS5 SDK
# and does the link, PS5 conversion and signing.
#
#   make objects        compile everything into build/obj
#   make print-objects  list the objects, for the link step

CC  ?= prospero-clang
CXX ?= prospero-clang++
TFLAGS ?=
HB ?= /opt/ps5-payload-sdk/target/user/homebrew

OBJDIR := build/obj

# EVO_* switches select the engine's app-module paths (sceAgc present,
# sceVideodec2 incl. 10-bit). NUVIO_APP marks the places this app changed.
DEFS := -DNUVIO_APP=1 -DEVO_APP_MODULE=1 -DEVO_AGC_DEVICE=1 -DEVO_AGC_HAVE_VIDEO_PIPES=1 -DEVO_TARGET_PS5=1 \
        -DEVO_VDEC_NATIVE_10BIT=1 -DEVO_NUVIO_EDITION=1 -DPP_4K_PRODUCT_STAGE=4 \
        -DEVO_PLAYER_VERSION=\"nuvio\"

INCS := -Isrc -Isrc/shim -Ithird_party \
        -Iengine/core/include -Iengine/include -Iengine/pp/include \
        -Iengine/media/include -Iengine/addons/include -Iengine/shaders/agc \
        -I$(HB)/include -I$(HB)/include/freetype2 -I$(HB)/include/harfbuzz \
        -I$(HB)/include/fribidi

CFLAGS   := -O2 -g -Wall -Wno-unused-parameter $(TFLAGS) $(DEFS) $(INCS)
CXXFLAGS := -O2 -g -Wall -Wno-unused-parameter -std=c++17 $(TFLAGS) $(DEFS) $(INCS)

ENGINE_C := $(wildcard engine/media/src/*.c) $(wildcard engine/pp/src/*.c) \
            $(wildcard engine/src/*.c) engine/addons/src/cJSON.c
ENGINE_CXX := engine/core/src/services/PlaybackController.cpp
APP_C   := $(wildcard src/*.c)
APP_CXX := $(wildcard src/*.cpp)

OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(ENGINE_C) $(APP_C)) \
        $(patsubst %.cpp,$(OBJDIR)/%.o,$(ENGINE_CXX) $(APP_CXX))

# Objects depend on the flag set: a changed -D (EVO_AGC_HAVE_VIDEO_PIPES once
# went missing this way) rebuilds everything instead of linking stale code.
FLAGS_STAMP := $(OBJDIR)/.flags
$(shell mkdir -p $(OBJDIR); echo '$(CFLAGS) | $(CXXFLAGS)' | cmp -s - $(FLAGS_STAMP) || \
        echo '$(CFLAGS) | $(CXXFLAGS)' > $(FLAGS_STAMP))

# Fonts, icons and artwork are .incbin'd into ui_assets.o (scripts/gen_assets.py).
-include build-assets.mk
$(OBJDIR)/src/ui_assets.o: $(ASSET_FILES)

objects: $(OBJS)

print-objects:
	@echo $(abspath $(OBJS))

$(OBJDIR)/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(OBJDIR)/%.o: %.cpp $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

clean:
	rm -rf build

-include $(OBJS:.o=.d)

.PHONY: objects print-objects clean
