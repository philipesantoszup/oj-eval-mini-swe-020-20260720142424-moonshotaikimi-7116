.PHONY: all
all:
	gcc -Wno-error=int-conversion -o code main.c buddy.c
