#!/bin/sh
# dist.sh: makes build/dist/bober.tar.gz, bober to unpack anywhere: bin/bober, built -release; lib/, the bundle of builtin/
# and std/ it writes, with the C they link compiled (src/root.bob); and the C sysroot, build/musl, but C++'s headers. Runs
# in the repo.
set -e
build/bober build -release src/bober.bob
rm -rf build/dist
mkdir -p build/dist/bober/bin
cp build/bober build/dist/bober/bin/
build/bober bundle build/dist/bober/lib
{
	find build/musl/usr/include -path '*/c++' -prune -o -type f -print
	find build/musl/usr/lib -name '*.[ao]'
} | tar -cf - -T - | tar -xf - -C build/dist/bober
tar -czf build/dist/bober.tar.gz -C build/dist bober
