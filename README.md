# Bober

Bober is a statically typed, ahead-of-time compiled, garbage-collected language that looks almost like TypeScript. The compiler, formatter, linter, test runner, documentation generator, and language server all live in one self-hosted binary.

Website and docs: [bober-lang.dev](https://bober-lang.dev).

```bober
interface Shape {
    area(): f64
}

class Rect {
    w = 1.0
    h = 1.0

    area(): f64 { return this.w * this.h }
}

class Box {
    ...Rect // fields and methods embedded by value

    label = ""
}

function total(shapes: Shape[]): f64 {
    let sum = 0.0
    for (const s of shapes) {
        sum += s.area()
    }
    return sum
}

test "structural interfaces, embedding, and call-site async" {
    const a = new Box()
    a.w = 3.0
    a.h = 4.0
    const job = async total([a, new Rect()])
    assert await job == 13.0
}
```

Bober is built for a fast build-test loop from the ground up. Parsing and typechecking work in linear time, and for unoptimized builds bober uses [TPDE](src/backend/tpde/) as a fast single-pass backend instead of going through LLVM IR, keeping generics unmonomorphized via runtime type descriptors. Bober also uses its own incremental [linker](src/link/) (ELF, Mach-O, and PE on x86-64 and arm64) that patches only the changed modules in place inside a kept shared-library image, so editing a file only recompiles and relinks what actually changed.

For optimized builds (`-O1`, `-O2`, `-release`), bober performs whole-program [monomorphization](src/mono.bob), lowering interfaces and structural object types to closed-world tagged unions, and compiles through [LLVM](src/backend/llvm.bob) for Go-like performance. It also targets WebAssembly GC (`-wasm`, [src/wasm.bob](src/wasm.bob), highly experimental) and ES2020 JavaScript (`-js`, [src/js.bob](src/js.bob), highly experimental).

Garbage collection is kept behind a narrow compiler-runtime boundary so that the language does not depend on a single collection strategy and can support different GCs in theory (and delegate directly to the host collector on WasmGC and JS). Backends emit precise stack maps for root scanning and write barriers on reference stores, while objects carry no per-object header. The current native [collector](src/gc.c) is a precise, generational, moving BiBoP GC over 64 KiB size-classed pages, with out-of-band ref and mark/forwarding bitmaps, a 512-byte card-table write barrier, and read-only frozen pages for top-level `const`s. Concurrency uses uncolored call-site `async`/`await` on stackful [fibers](src/fiber.c) over `epoll`/`kqueue`.

The compiler and [standard library](std/) are written in Bober and [bootstrapped](boot/README.md).

## Deterministic and Fast Testing

Tests are built into the language (`test` blocks and value-diffing `assert`) and run in a deterministic virtual environment by default. Clocks use logical time, so sleeping or waiting on a timeout never burns wall-clock time: once all fibers are parked, time jumps straight to the next deadline. TCP and UDP sockets run in-process over an embedded lwIP loopback stack driven by that same logical clock, so network and HTTP tests do not touch OS sockets or collide on ports. Randomness (including `std/crypto/random`) is seeded deterministically, environment variables are hidden, and each test gets its own temporary directory.

Because tests are deterministic, `bober test` caches passing results automatically. The runtime tracks every file a test reads outside its temporary directory and taints tests that depend on external side effects; untainted passing tests are cached by their compiled code hash and read-file hashes, so unchanged tests are skipped on subsequent runs. Tests that do need to run execute in parallel across cores.

## Interop

Bober includes a built-in [C11 frontend](src/c/) that parses C headers directly and compiles C dependencies against a bundled musl sysroot on Linux (or the system SDK on macOS):

```bober
import { labs } from "stdlib.h"

function main() {
    print(labs(-42))
}
```

When targeting JavaScript (`-js`, highly experimental), imports tagged `@TypeScript` read `.d.ts` typings directly ([src/dts.bob](src/dts.bob)) and automatically await `Promise`-returning functions:

```bober
import { getInput } from "@actions/core" @TypeScript
import { restoreCache } from "@actions/cache" @TypeScript

function main() {
    const hit = restoreCache(["build"], `deps-${getInput("key")}`)
    print(hit ?? "miss")
}
```

## Install

Prebuilt binaries are available for Linux x86-64, Linux arm64, and macOS arm64:

```sh
curl -fsSL https://bober-lang.dev/install.sh | sh
bober run hello.bob
bober test
```

See [boot/README.md](boot/README.md) for building from source. Bober is young and unstable; MIT licensed ([LICENSE](LICENSE)).
