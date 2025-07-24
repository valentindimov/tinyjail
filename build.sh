#!/bin/bash
set -e
mkdir -p build
musl-gcc -Wall -pedantic-errors -s -static -fvisibility=hidden -fPIC -shared src/tinyjail.c -o build/libtinyjail.so
musl-gcc -Wall -pedantic-errors -s -static src/tinyjail.c src/main.c -o build/tinyjail