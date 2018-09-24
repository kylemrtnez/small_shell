CC=gcc
CFLAGS=-Wall
CFLAGS+=-g

smallsh: smallsh.c
	$(CC) $(CFLAGS) -o $@ $<
