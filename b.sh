#!/bin/sh
yasm -f elf64 -o loops.o loops.asm && gcc -g -Wall -pedantic -std=c99 -O2 -o test.o -c test.c && gcc -g -o test test.o loops.o -lpthread

