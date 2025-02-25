CC = gcc
CFLAGS = -Wextra -Wall -g
IFLAGS = 
LFLAGS = -L lib
LIBS += -lraylib -lm -lavcodec -lavformat -lavutil -lswscale
LIBS += -lraylib -pthread -ldl -lm -lavcodec -lavformat -lavutil -lswscale -lswresample

BUILD_RAYLIB ?= FALSE
VENDOR_FFMPEG ?= FALSE

ifeq ($(BUILD_RAYLIB), TRUE)
	IFLAGS += -I lib/raylib/src
	DEPS += raylib
endif
ifeq ($(VENDOR_FFMPEG), TRUE)
	IFLAGS += -I lib/ffmpeg/include
	LFLAGS += -L lib/ffmpeg/lib
	LIBS += -lswresample
	DEPS += ffmpeg
endif

all: $(DEPS) jplay

# DOWNLOAD FFMPEG
FFMPEG_BUILD = ffmpeg-master-latest-linux64-gpl-shared
FFMPEG_URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$(FFMPEG_BUILD).tar.xz"
ffmpeg:
	@if [ ! -d lib/ffmpeg/lib ]; then \
		echo DOWLOADING FFMPEG...; \
		mkdir -p lib/ffmpeg; \
		set -xe && wget -q -P lib/ffmpeg $(FFMPEG_URL); \
		tar -xf lib/ffmpeg/$(FFMPEG_BUILD).tar.xz \
			--strip-components=1 -C lib/ffmpeg $(FFMPEG_BUILD)/lib $(FFMPEG_BUILD)/include; \
		rm lib/ffmpeg/$(FFMPEG_BUILD).tar.xz; \
		echo "#!/bin/sh\nexport LD_LIBRARY_PATH=lib/ffmpeg/lib\nexec ./jplay \$$@" > run.sh; \
		chmod +x run.sh; \
	fi
	echo "#!/bin/sh\nexport LD_LIBRARY_PATH=lib/ffmpeg/lib\nexec ./jplay \$$@" > run.sh;

# RAYLIB
lib/libraylib.a:
	@if [ ! -d lib/raylib/src ]; then \
		echo raylib submodule not found && exit 1; \
	fi
	@echo BUILDING RAYLIB...
	@cd lib/raylib/src && make
	cp lib/raylib/src/libraylib.a lib

raylib: lib/libraylib.a
	@if [ ! -d lib/raylib/src ]; then \
		echo raylib submodule not found && exit 1; \
	fi

jplay: player.c
	$(CC) -o jplay $< $(CFLAGS) $(IFLAGS) $(CFLAGS) $(LFLAGS) $(LIBS)
