#!/bin/sh
# dist.sh: makes build/dist/bober.tar.gz, bober to unpack anywhere: bin/bober, built -release, and the files of its root
# (src/root.bob) as the repo lays them out. Runs in the repo.
set -e
build/bober build -release src/bober.bob
rm -rf build/dist
mkdir -p build/dist/bober/bin
cp build/bober build/dist/bober/bin/
{
	git ls-files builtin std vendor/mbedtls vendor/monocypher src/rt.h src/rt.c src/gc.c src/fiber.c src/io.c src/type.c |
		grep -E '\.(bob|pkg|c|h)$' | grep -v '\.test\.bob$'
	find build/musl/usr/include -path '*/c++' -prune -o -type f -print
	find build/musl/usr/lib -name '*.[ao]'
} | tar -cf - -T - | tar -xf - -C build/dist/bober
tar -czf build/dist/bober.tar.gz -C build/dist bober
