#!/bin/bash
set -e
mkdir -p build
musl-gcc -Wall -pedantic-errors -s -static -fvisibility=hidden -fPIC -shared *.c -o build/libtinyjail.so
musl-gcc -Wall -pedantic-errors -s -static *.c -o build/tinyjail