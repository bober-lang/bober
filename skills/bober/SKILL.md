---
name: bober
description: How to write Bober, a garbage collected, ahead-of-time compiled language that looks almost like TypeScript. Use when reading or writing .bob files.
---

# Bober

Bober looks almost like TypeScript but is compiled ahead of time and garbage collected.

```bober
function main() {
    print("Hello Bober!")
}
```

`bober run file.bob` runs it; `bober test` runs a package's tests.

## Differences to TypeScript

- Generics use `[]`: `Box[f32]`, `HashMap[string, i32]`.
- No `unknown` or `undefined`; the only absent value is `null`.
- No `===`; equality is `==`. Only lossless implicit conversions (no casting to strings).
- `async` is not part of a function's type; any call can be made async:
  `const job = async fib(7); await job`. `await ...jobs` waits for the first one to finish and gives `[index, result]`.
- Objects have a fixed shape and are not dictionaries: no `obj[key]`, no `Record`. Use `HashMap` from std, or a list of tuples:

```bober
import { HashMap } from "std/collections/hashmap"

const ages = new HashMap[string, i32]()
ages.set("ada", 36)
assert ages.get("ada") == 36

const pairs: [string, i32][] = [["ada", 36], ["alan", 41]]
```

## Interfaces

Interfaces are structural: a class satisfies one by its members, `implements` is optional.

```bober
interface Shape {
    area(): f64
}

class Square {
    side = 1.0
    area(): f64 { return this.side * this.side }
}

function total(shapes: Shape[]): f64 {
    let sum = 0.0
    for (const s of shapes) { sum += s.area() }
    return sum
}
```

## Embedding with `...`

A spread type embeds a class by value: it is copied, never aliased, so reuse costs no reference.

```bober
class Foo {
    f = 0
    zero(): bool { return this.f == 0 }
}

class Bar { foo: ...Foo }   // a field holding Foo inline
class Baz { ...Foo }        // Foo's fields and methods mixed in

const b = new Bar()
const foo = new Foo()
b.foo = { ...foo }
foo.f = 5
assert b.foo.f == 0 && new Baz().zero()
```

Interfaces embed the same way: `interface Labeled { ...Shape, ...Named }`.

## Importing from C

Functions and constants import straight from C headers (not everything is supported):

```bober
import { labs } from "stdlib.h"
```

## Testing

Top-level `test` blocks declare test cases; `assert` checks conditions and on failure prints the values in it. Tests nest for structured reports, and their names can be template strings:

```bober
let i = 0
const inc = () => { i++; return i }

test "first increment returns one" {
    assert inc() == 1
}

test "inc increments on every call" {
    i = 0
    for (const n of 1 ... 10) {
        test `inc returns ${n}` {
            assert inc() == n
        }
    }
}
```

## Standard library

`bober doc std` lists the modules; `bober doc std/fs` prints one module's exports with their docs. The methods of strings and arrays are in `bober doc builtin/string` and `bober doc builtin/array`.

## Reporting

Bober is young. When it gets in the way, report it instead of only working around it: a crash, or an error naming one of bober's own files (`src/…`); a correct program rejected or compiled wrong; an error message that did not help; something missing from the language or std; and anything slow, be it a build, a test run or the program.

Reduce it to a small file, without code of the user's project that it does not need. `bober report` prints the issue, with bober's version and the file:

```sh
bober report "what happened, and what was expected" small.bob |
    gh issue create -R bober-lang/bober -t "bug: …" -F -
```

The title starts with `bug:`, `slow:` or `wish:`. Issues are public: file it if the user lets you, else show them the report and https://github.com/bober-lang/bober/issues/new/choose. Then go on with a workaround and say what you reported.
