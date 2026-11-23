//! The launcher of images, a debug build's programs as shared libraries (src/link): `launch <image> <entry>[@<version>]
//! args...` loads the image and returns what its entry returns, called as C's main with the args, the entry's name, and
//! version if given, first, by which the program knows the code it runs (src/lift.bob's compilerId). It is run through
//! the C library's loader, `<sysroot>/usr/lib/libc.so launch ...`, so that it holds no path to one, but on macOS, whose
//! loader is the system's. An image named `*.run` is a run's own link to it, removed once loaded. The image is locked
//! shared while it runs, as a build writes one in place only if none has it (src/bober.bob). On Linux its malloc,
//! mimalloc, is the images' and the C library's, as it exports it.

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <spawn.h>
#else
#include <sys/personality.h>
#endif

/// Runs this launcher again without ASLR, if it can and has not: tests' addresses, and so the ids of what is outside the
/// heap (src/gc.c), are then the same each run. The processes they start inherit it on Linux.
static void fixed(int argc, char **argv) {
	extern char **environ;
#ifdef __APPLE__
	char path[PATH_MAX];
	uint32_t n = sizeof path;
	posix_spawnattr_t a;
	if (getenv("BOBER_ASLR")) {
		unsetenv("BOBER_ASLR"); // the run again, once, though the system ran it with ASLR still
		return;
	}
	if (_NSGetExecutablePath(path, &n) || posix_spawnattr_init(&a)) return;
	setenv("BOBER_ASLR", "off", 1);
	posix_spawnattr_setflags(&a, POSIX_SPAWN_SETEXEC | 0x100); // _POSIX_SPAWN_DISABLE_ASLR
	posix_spawn(0, path, 0, &a, argv, environ);
#else
	int p = personality(0xffffffff);
	if (p == -1 || p & ADDR_NO_RANDOMIZE || personality(p | ADDR_NO_RANDOMIZE) == -1) return;
	char **v = malloc((argc + 2) * sizeof *v); // the loader's, which runs this one, then this one's
	v[0] = "ld.so", memcpy(v + 1, argv, (argc + 1) * sizeof *v);
	execve("/proc/self/exe", v, environ);
#endif
}

int main(int argc, char **argv) {
	if (argc >= 3 && !strncmp(argv[2], "bober.tests.", 12)) fixed(argc, argv);
	char path[PATH_MAX]; // absolute: the package's launcher, hardened on macOS (ci.yml), loads no relative path
	if (argc >= 3) flock(open(argv[1], O_RDONLY | O_CLOEXEC), LOCK_SH);
	void *image = argc < 3 ? 0 : dlopen(realpath(argv[1], path) ? path : argv[1], RTLD_NOW);
	int (*entry)(int, char **) = 0;
	if (image) entry = (int (*)(int, char **))dlsym(image, strndup(argv[2], strcspn(argv[2], "@")));
	const char *run = argc < 3 ? 0 : strrchr(argv[1], '.');
	if (run && !strcmp(run, ".run")) {
		remove(argv[1]);
	}
	if (!entry) {
		fprintf(stderr, "launch: %s\n", argc < 3 ? "usage: launch <image> <entry>[@<version>] args..." : dlerror());
		return 127;
	}
	return entry(argc - 2, argv + 2);
}
