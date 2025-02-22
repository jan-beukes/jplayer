CC = gcc
CFLAGS = -Wextra -Wall -g
LDLIBS = -lraylib -lm -lavcodec -lavformat -lavutil -lswscale

player: player.c
	$(CC) -o player $^ $(CFLAGS) $(LDLIBS)
