# Builds the sysroot that bober links its programs against, statically, in build/musl: musl with the host's Linux
# headers, then libunwind, libc++abi and libc++ built against it by the system's clang, from the LLVM of bober.pkg, and
# that clang's compiler-rt builtins, since bobcc links against the sysroot alone. Run as make -C src/backend/llvm.
root := $(abspath ../../..)
out := $(root)/build
sysroot := $(out)/musl
src := $(out)/llvm-runtimes
triple := $(shell uname -m)-linux-musl
llvm := https://github.com/llvm/llvm-project/archive/6989f0e555cb9ba5bf85858934063b1a08b8324a.tar.gz

$(sysroot)/usr/lib/libc++.a: $(sysroot)/usr/lib/libc.a
	mkdir -p $(src) && curl -sSL $(llvm) | tar xz -C $(src) --strip-components=1
	cmake -G Ninja -S $(src)/runtimes -B $(src)/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
		-DCMAKE_C_COMPILER_TARGET=$(triple) -DCMAKE_CXX_COMPILER_TARGET=$(triple) -DCMAKE_ASM_COMPILER_TARGET=$(triple) \
		-DCMAKE_SYSROOT=$(sysroot) -DCMAKE_INSTALL_PREFIX=$(sysroot)/usr -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
		-DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
		-DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_USE_COMPILER_RT=ON \
		-DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
		-DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON -DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=ON -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
		-DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_HAS_MUSL_LIBC=ON \
		-DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON -DLLVM_INCLUDE_TESTS=OFF
	ninja -C $(src)/build install-unwind install-cxxabi install-cxx
	cp $(shell clang -print-libgcc-file-name -rtlib=compiler-rt) $(sysroot)/usr/lib/libclang_rt.builtins.a
	rm -rf $(src)

$(sysroot)/usr/lib/libc.a:
	mkdir -p $(sysroot)
	cd $(sysroot) && $(root)/vendor/musl/configure --prefix=/usr CC=clang > /dev/null
	$(MAKE) -C $(sysroot) -s DESTDIR=$(sysroot) install
	cp -rL /usr/include/linux /usr/include/asm-generic /usr/include/$(shell uname -m)-linux-gnu/asm $(sysroot)/usr/include
