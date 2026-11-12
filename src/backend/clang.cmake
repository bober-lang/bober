# How bober.pkg builds bober's C++, after bober's defaults: by clang, with the sysroot's libc++, on macOS its SDK's, rather
# than clang's own headers, which it finds first, linking by LLD with compiler-rt's builtins and libunwind.
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_FLAGS_INIT "-stdlib++-isystem${CMAKE_SYSROOT}${CMAKE_OSX_SYSROOT}/usr/include/c++/v1")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++")
