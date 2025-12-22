#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

mkdir -p build
musl-gcc -O2 -Wall -pedantic-errors -s -static -fvisibility=hidden -fPIC -shared src/lib/*.c -o build/libtinyjail.so
musl-gcc -O2 -Wall -pedantic-errors -s -static src/lib/*.c src/main.c -o build/tinyjail
