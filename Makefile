CC = gcc
CFLAGS = -Wextra -Wall -g
LDLIBS = -lraylib -lm -lavcodec -lavformat -lavutil -lswscale

j-play: player.c
	$(CC) -o j-play $^ $(CFLAGS) $(LDLIBS)
