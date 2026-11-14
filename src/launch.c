//! The launcher of images, a debug build's programs as shared libraries (src/link): `launch <image> <entry> args...`
//! loads the image and returns what its entry returns, called as C's main with the args, the entry's name first. It is
//! run through the C library's loader, `<sysroot>/usr/lib/libc.so launch ...`, so that it holds no path to one, but
//! on macOS, whose loader is the system's. An image named `*.run` is a run's own link to it, removed once loaded. The
//! image is locked shared while it runs, as a build writes one in place only if none has it (src/bober.bob).

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>

int main(int argc, char **argv) {
	char path[PATH_MAX]; // absolute: the package's launcher, hardened on macOS (ci.yml), loads no relative path
	if (argc >= 3) flock(open(argv[1], O_RDONLY | O_CLOEXEC), LOCK_SH);
	void *image = argc < 3 ? 0 : dlopen(realpath(argv[1], path) ? path : argv[1], RTLD_NOW);
	int (*entry)(int, char **) = image ? (int (*)(int, char **))dlsym(image, argv[2]) : 0;
	const char *run = argc < 3 ? 0 : strrchr(argv[1], '.');
	if (run && !strcmp(run, ".run")) {
		remove(argv[1]);
	}
	if (!entry) {
		fprintf(stderr, "launch: %s\n", argc < 3 ? "usage: launch <image> <entry> args..." : dlerror());
		return 127;
	}
	return entry(argc - 2, argv + 2);
}
