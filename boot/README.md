# bober v0 bootstrap interpreter

A tree-walking interpreter for the Bober subset needed to self-host the compiler.
`lex.l` (flex) tokenizes the whole file, then a filter turns significant newlines into `;` (spec 2.2) and marks arrow `(` and `=> {`.
`parse.y` (bison, LALR, no conflicts) builds the AST and resolves locals to frame slots while parsing, a frame per call and per loop iteration. `interp.c` evaluates it.

Build: `make` (needs bison 3.8, flex 2.6, Boehm's libgc and the system's LLVM, which it links) produces `../build/boot`, which bootstraps the native bober from the repo's
root, after `make -C src/backend/llvm` builds its sysroot, with `build/boot run src/bober.bob build -release -I "$(llvm-config --includedir)" src/bober.bob`, into
`build/bober`, building the C libraries of `bober.pkg` with CMake (`-release`: boot has LLVM but not TPDE; `-I`: boot binds the
system's LLVM, as those are not built yet).

    bober run <file> [args...]   initialize top-level vars, call main(); exit status = main's result
    bober test <file> [args...]  run each top-level test: `ok <name>` or `FAIL <name>: <trap>`
                                a failing subtest prints `FAIL <test> / <sub>: <trap>` and its siblings still run;
                                a failed `assert a == b` shows both sides

Supported: the full surface grammar except async/await/spawn. That covers modules (named and `* as` imports),
lazily initialized top-level vars with a cycle trap, numbers with a runtime kind (wrapping, unsigned ops, `as`),
strings, fixed-length arrays and tuples, objects, classes (fields, methods, constructor, statics),
interfaces and type aliases as runtime type values, unions with dynamic tags, closures, default and rest parameters,
spread, destructuring, switch and if expressions, `?.`, `??`, `!`, try/catch/finally, test and assert.
Types are ordinary expressions; they are evaluated where they change behavior: conversions on store, zero values,
`as`, `instanceof`, case labels, catch. Reflection covers `kind`, `name`, `fields`, `elem`, `length`, `members`,
`value`, `tags` of classes and class fields, `base` and `args` on a tag (a class not generic is its own `base`), and `v[name]` reads a field. A class's type parameter is its argument or default, one class per instantiation; a function's not given is that of the value of the first param of its type, else erased to `Type`, so `T.kind` is `"type"`; `typeof x` is the
type of x's value, not its static type. A class's statics shadow its reflection members.

An import from a `.h` or `.c` runs `src/c/bind.bob` on the file when a name is first used, which declares the names as a
module whose functions and objects have no body; functions call C through libffi and objects read their value, looked up in the process (load other libraries
with `DYLD_INSERT_LIBRARIES`). Headers are found from the importing file, then in the directories of the
program's `-I` flags, as a C compiler takes them, then in those of the C libraries of the importer's `bober.pkg`.
Classes of numbers, fixed arrays and embedded classes cross as C structs: by value as `...T`, else through a
pointer to a copy, which is copied back after the call; an array's elements and a string's bytes cross the same way to
a pointer param. A header's static functions are compiled with `cc` into a library that is loaded first; this needs
ELF's aliases.
Boot is macOS-only here; building on a target that is not little-endian LP64 fails.

Builtins: `print(v)`, `readFile(path): string | null`, `writeFile(path, data): bool`, `args(): string[]`,
`chr(c): string`, `fromBytes(bytes): string`, `targetOs()` and `targetArch()` for std/os, `toC`, `fromC`, `toCN`, `fromCN`, `peek` and `poke`
for std/c, builtin/mem's `addr` and `load[T]` for std/float, as boot provides that module itself (its intrinsics have no body), and the methods of strings and arrays the compiler uses: `slice`, `at`, `push`, `append`, `pop`, `join`, `map`, `filter`, `find`, `some`, `every` and `equals`. A builtin's type arg comes as its `self`.

Out of scope: type checking and static errors (it accepts more than the spec allows), async/await/spawn/isolates,
freezing, private and readonly in `fields`, names of types other than classes and primitives, embedding without a field
name, `throws` checking, string16/string32.
Known differences: `T[n]` is an ordinary reference array, an embedded field `a: ...T` is a reference to its own `new T()`, which `x.a = {...v}` copies into,
top-level declarations take a plain name, not a pattern, and the newline filter guesses 2.2 from the tokens around
a newline, so some line breaks the spec allows are rejected.
