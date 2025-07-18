#!/bin/bash
set -e
mkdir -p build
musl-gcc -Wall -pedantic-errors -s -static -fvisibility=hidden -fPIC -shared src/*.c -o build/libtinyjail.so
musl-gcc -Wall -pedantic-errors -s -static src/*.c -o build/tinyjail