#!/bin/sh
yasm -f elf -o loops.o loops.asm
gcc -O2 -o test test.c loops.o
