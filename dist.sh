#!/bin/sh
# dist.sh: makes build/dist/bober.tar.gz, bober to unpack anywhere: bin/bober, built -release; lib/, the bundle of
# builtin/ and std/ it writes, with the C they link compiled (src/root.bob), the launcher of images, and version, the
# line naming the commit (src/update.bob); their sources but tests, for bober doc and to read, and skills/; and the C
# sysroot, build/musl, but C++'s headers, with libc.so, which loads images; macOS's is the system's. Runs in the repo.
set -e
build/bober run -release src/bober.bob help > /dev/null
rm -rf build/dist
mkdir -p build/dist/bober/bin
cp build/bober build/dist/bober/bin/
build/bober bundle build/dist/bober/lib
git log -1 --format='bober %h %cs' > build/dist/bober/lib/version
find std builtin -name '*.bob' ! -name '*.test.bob' | tar -cf - -T - | tar -xf - -C build/dist/bober
cp -R skills build/dist/bober/
if [ "$(uname)" != Darwin ]; then
	{
		find build/musl/usr/include -path '*/c++' -prune -o -type f -print
		find build/musl/usr/lib -name '*.[ao]' -o -name libc.so
	} | tar -cf - -T - | tar -xf - -C build/dist/bober
fi
tar -czf build/dist/bober.tar.gz -C build/dist bober
