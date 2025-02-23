CC = gcc
CFLAGS = -Wextra -Wall -g
LDLIBS = -L lib
LDLIBS += -lraylib -lm -lavcodec -lavformat -lavutil -lswscale

BUILD_RAYLIB ?= TRUE

all: raylib jplay

raylib:
ifeq ($(BUILD_RAYLIB), TRUE)
	@if [ ! -d lib/raylib/src ]; then \
		echo raylib submodule not found; \
		echo use git submodule update --init; \
		exit 1; \
	fi
	
	cd lib/raylib/src && make
	cp lib/raylib/src/libraylib.a lib
endif

jplay: player.c
	$(CC) -o jplay $^ $(CFLAGS) $(LDLIBS)
