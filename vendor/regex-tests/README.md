# regex-tests

Regular expression cases from other projects' test suites, unmodified, which `std/regex.vendor.test.bob` reads.
License files sit next to the sources.

| File | Upstream | License |
|---|---|---|
| `go/re2-search.txt` | [src/regexp/testdata/re2-search.txt](https://github.com/golang/go/blob/52f1233d8c6a9e9eaf191a6bdbff16d9151eb5b8/src/regexp/testdata/re2-search.txt), logged by RE2 | BSD-3-Clause |
| `go/find_test.go` | [src/regexp/find_test.go](https://github.com/golang/go/blob/52f1233d8c6a9e9eaf191a6bdbff16d9151eb5b8/src/regexp/find_test.go) | BSD-3-Clause |
| `oniguruma/test_utf8.c` | [test/test_utf8.c](https://github.com/kkos/oniguruma/blob/f95747b462de672b6f8dbdeb478245ddf061ca53/test/test_utf8.c) | BSD-2-Clause |

Oniguruma's syntax is Ruby's, a superset of RE2's in the main; the harness runs only the cases whose pattern
`std/regex` compiles and whose meaning is RE2's. QuickJS's regexp tests (MIT) are left out: they are a few dozen
JavaScript asserts, nearly all on lookaround, the `v` flag and `\p`, which RE2's syntax does not have.
