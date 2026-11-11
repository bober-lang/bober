//! The launcher of images, a debug build's programs as shared libraries (src/link): `launch <image> <entry> args...`
//! loads the image and returns what its entry returns, called as C's main with the args, the entry's name first. It is
//! run through the C library's loader, `<sysroot>/usr/lib/libc.so launch ...`, so that it holds no path to one.

#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
	void *image = argc < 3 ? 0 : dlopen(argv[1], RTLD_NOW);
	int (*entry)(int, char **) = image ? (int (*)(int, char **))dlsym(image, argv[2]) : 0;
	if (!entry) {
		fprintf(stderr, "launch: %s\n", argc < 3 ? "usage: launch <image> <entry> args..." : dlerror());
		return 127;
	}
	return entry(argc - 2, argv + 2);
}
