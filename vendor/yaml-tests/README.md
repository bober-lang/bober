# yaml-tests

YAML's own test suite, unmodified, which `std/encoding/yaml.vendor.test.bob` reads. The license file sits next to it.

| Directory | Upstream | License |
|---|---|---|
| `yaml-test-suite` | [yaml/yaml-test-suite](https://github.com/yaml/yaml-test-suite/tree/data-2022-01-17) at tag `data-2022-01-17`, its generated data | MIT |

Each case is a directory, or a directory of numbered ones: `===` names it, `in.yaml` is the input, `test.event` the
parse events it must give, `in.json` the values it loads to, `out.yaml` and `emit.yaml` its canonical re-emit, and an
`error` file marks input that must be rejected. The `name` and `tags` symlink indexes are left out.
