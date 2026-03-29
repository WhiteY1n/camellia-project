CC := gcc
CFLAGS := -Wall -Wextra -O2

all: crypto_mouse_cli

crypto_mouse_cli: crypto_mouse_cli.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f crypto_mouse_cli
