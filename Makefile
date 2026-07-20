.PHONY: all
all:
	gcc -Wno-error=int-conversion -o test main.c buddy.c
