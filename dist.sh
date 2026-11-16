#!/bin/sh
# dist.sh: makes build/dist/bober.tar.gz, bober to unpack anywhere: bin/bober, built -release; lib/, the bundle of
# builtin/ and std/ it writes, with the C they link compiled (src/root.bob), the launcher of images, and version, the
# line naming the commit (src/update.bob); skills/; and the C sysroot, build/musl, but C++'s headers, with libc.so,
# which loads images; macOS's is the system's. With LICENSE, and NOTICES, the licences of the code of others it holds:
# each as it lies in vendor/, or in vendor/NOTICES if its code is fetched (bober.pkg) or ported into std/, and on Linux
# the host's of the sysroot's Linux headers. Runs in the repo.
set -e
notice() { printf '== %s\n\n' "$1"; shift; cat "$@"; echo; }
build/bober run -release src/bober.bob help > /dev/null
rm -rf build/dist
mkdir -p build/dist/bober/bin
cp build/bober build/dist/bober/bin/
build/bober bundle build/dist/bober/lib
git log -1 --format='bober %h %cs' > build/dist/bober/lib/version
cp -R skills LICENSE build/dist/bober/
{
	notice "Mbed TLS, Apache-2.0 OR GPL-2.0-or-later" vendor/mbedtls/LICENSE
	notice "Monocypher, BSD-2-Clause OR CC0-1.0" vendor/monocypher/LICENCE.md
	notice "lwIP, BSD-3-Clause" vendor/lwip/COPYING
	notice "TPDE, Apache-2.0 WITH LLVM-exception" vendor/tpde/LICENSES/Apache-2.0.txt vendor/tpde/LICENSES/LLVM-exception.txt
	notice "fadec, of TPDE, BSD-3-Clause" vendor/tpde/deps/fadec/LICENSE
	notice "disarm, of TPDE, BSD-3-Clause" vendor/tpde/deps/disarm/LICENSE
	notice "musl, MIT" vendor/musl/COPYRIGHT
	notice "Go, BSD-3-Clause" vendor/parse-tests/go/LICENSE
	cat vendor/NOTICES
} > build/dist/bober/NOTICES
if [ "$(uname)" != Darwin ]; then
	{
		find build/musl/usr/include -path '*/c++' -prune -o -type f -print
		find build/musl/usr/lib -name '*.[ao]' -o -name libc.so
	} | tar -cf - -T - | tar -xf - -C build/dist/bober
	{
		notice "mimalloc, MIT" vendor/mimalloc/LICENSE
		notice "Linux's headers, GPL-2.0 WITH Linux-syscall-note" /usr/share/doc/linux-libc-dev/copyright \
			/usr/share/common-licenses/GPL-2
	} >> build/dist/bober/NOTICES
fi
tar -czf build/dist/bober.tar.gz -C build/dist bober
