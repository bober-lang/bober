# parse-tests

Integer and float parsing cases from other projects' test suites, reduced to data under bober's strict semantics.
Each file defines arrays of `int_case` or `float_case` (`cases.h`), which `std/parse.vendor.test.bob` imports from it.
`make` builds `build/libparsetests.dylib`, which holds them.

Each row is a scan from index 0:
- `OK`: `at` is the index after the number.
- `SYNTAX`: no number; `at` is where it was expected (0, or 1 after a `-` the type accepts).
- `RANGE`: `at` is 0.

`value` is checked only for `OK`, and a NaN matches any NaN.

A whole-string parse is ok iff the scan is `OK` and `at == len`.
Rows are narrow unless the input is inherently wide. Once bober has `string16` and `string32`, the harness reruns every narrow row as UTF-16 and UTF-32 by zero-extending its code units; until then it skips wide rows.

Strict means:
- Integers: `-?digits` in an explicit radix 2..36; `-` on an unsigned type is `SYNTAX` at 0.
- Floats: `-?(inf | D+(.D+)?([eE][+-]?D+)?) | nan`, correctly rounded straight to the target type. Overflow is `RANGE`; underflow rounds to a subnormal or ±0.

How upstream cases were adapted:
- Cases that relied on leniency keep their input and get the strict expectation. That covers whitespace, `+`, prefixes, base 0, `_`, `infinity`, hex floats and inf/nan in any case. Repeats of one kind of leniency are cut to a few rows.
- A leading `.` that only carried a rounding test (musl) becomes `0.`, keeping the value test.
- Cases that need an invalid radix, custom bounds, long double, rounding modes or locales are dropped, and so are duplicates.

The upstream files are in commit `bd3c06d`; diff against it to see every change.
License headers are kept. NetBSD and OpenBSD files carry their own; the other projects' license files sit next to their sources.

| File | Upstream | License |
|---|---|---|
| `openbsd/strtoltest.c` | [regress/lib/libc/strtol/strtoltest.c](https://github.com/openbsd/src/blob/2114d7b7c8fbfd8d285dd7b5b5eb44e1f6b70f55/regress/lib/libc/strtol/strtoltest.c) | ISC |
| `openbsd/strtonumtest.c` | [regress/lib/libc/strtonum/strtonumtest.c](https://github.com/openbsd/src/blob/2114d7b7c8fbfd8d285dd7b5b5eb44e1f6b70f55/regress/lib/libc/strtonum/strtonumtest.c) | ISC |
| `netbsd/t_strtol.c` | [tests/lib/libc/stdlib/t_strtol.c](https://github.com/NetBSD/src/blob/5b0e0d5997518246c017c6577adcb59be12d0a79/tests/lib/libc/stdlib/t_strtol.c) | BSD-2-Clause |
| `netbsd/t_strtoi.c` | [tests/lib/libc/stdlib/t_strtoi.c](https://github.com/NetBSD/src/blob/5b0e0d5997518246c017c6577adcb59be12d0a79/tests/lib/libc/stdlib/t_strtoi.c) | BSD-2-Clause |
| `netbsd/t_wcstod.c` | [tests/lib/libc/locale/t_wcstod.c](https://github.com/NetBSD/src/blob/5b0e0d5997518246c017c6577adcb59be12d0a79/tests/lib/libc/locale/t_wcstod.c) | BSD-2-Clause |
| `freebsd/sscanf_test.c` | [lib/libc/tests/stdio/sscanf_test.c](https://github.com/freebsd/freebsd-src/blob/775e421dc38f18fa6b7fdc7b214a013cca44b58b/lib/libc/tests/stdio/sscanf_test.c) | BSD-2-Clause |
| `musl-libc-test/strtod.c` | [src/functional/strtod.c](https://repo.or.cz/libc-test.git/blob/7b95dfa5f5d5ca4d949221e0228ccc290bacc14e:/src/functional/strtod.c), strtold.c | MIT |
| `musl-libc-test/strtof.c` | [src/functional/strtof.c](https://repo.or.cz/libc-test.git/blob/7b95dfa5f5d5ca4d949221e0228ccc290bacc14e:/src/functional/strtof.c) | MIT |
| `fast_float/basictest.c` | [tests/basictest.cpp](https://github.com/fastfloat/fast_float/blob/0de7a21da582b6c84b0546772a96549882905d2b/tests/basictest.cpp) | MIT |
| `fast_float/json_fmt.c` | [tests/json_fmt.cpp](https://github.com/fastfloat/fast_float/blob/0de7a21da582b6c84b0546772a96549882905d2b/tests/json_fmt.cpp) | MIT |
| `double-conversion/test-strtod.c` | [test/cctest/test-strtod.cc](https://github.com/google/double-conversion/blob/71b329c21e8255643e9c5a23e1013c6eba4f604b/test/cctest/test-strtod.cc) | BSD-3-Clause |
| `double-conversion/test-conversions.c` | [test/cctest/test-conversions.cc](https://github.com/google/double-conversion/blob/71b329c21e8255643e9c5a23e1013c6eba4f604b/test/cctest/test-conversions.cc) | BSD-3-Clause |
| `go/testfp.c` | [src/internal/strconv/testdata/testfp.txt](https://github.com/golang/go/blob/f723e21aaa1c25089a9524de1947d3c06b434c19/src/internal/strconv/testdata/testfp.txt) | BSD-3-Clause |
| `go/atoi.c` | [internal/strconv/atoi_test.go](https://github.com/golang/go/blob/f723e21aaa1c25089a9524de1947d3c06b434c19/src/internal/strconv/atoi_test.go), [strconv/number_test.go](https://github.com/golang/go/blob/f723e21aaa1c25089a9524de1947d3c06b434c19/src/strconv/number_test.go) | BSD-3-Clause |
| `go/atof.c` | [internal/strconv/atof_test.go](https://github.com/golang/go/blob/f723e21aaa1c25089a9524de1947d3c06b434c19/src/internal/strconv/atof_test.go), [strconv/number_test.go](https://github.com/golang/go/blob/f723e21aaa1c25089a9524de1947d3c06b434c19/src/strconv/number_test.go) | BSD-3-Clause |
