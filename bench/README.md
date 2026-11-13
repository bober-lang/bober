# Benchmarks against Go

Each program exists twice, as `<name>.bob` and `go/<name>/main.go`, doing the same work and printing the same checksum.

    build/bober run -release bench/<name>.bob
    (cd bench/go && go build -o ../../build/go-<name> ./<name>) && build/go-<name>

`httpd` is a server; load it with `build/go-load -addr http://127.0.0.1:<port> -mode ping|orders -c 32 -d 3s`.
Bober serves on one core; compare it with `GOMAXPROCS=1 build/go-httpd <port>`.

Results on 2026-11-03, arm64, go1.26, ms (lower is better). Bober runs on one core; Go is shown with
`GOMAXPROCS=1`, which makes its collector share that core too, and with all 8 cores.

| benchmark | bober | go, 1 core | go, 8 cores | note |
|---|---|---|---|---|
| hanoi-count | 135 | 221 | 219 | recursion |
| hanoi-stacks | 43 | 41 | 42 | push and pop |
| sort-quick-i32 | 253 | 260 | 261 | int array indexing |
| sort-merge-objects | 512 | 511 | 319 | 324 once Item is held as a ref (sortref.bob); Go with Item by value (go/sortval): 953 / 622 |
| sort-std-objects | 472 | 238 | 240 | std/algo stableSort vs slices.SortStableFunc |
| binarytrees | 494 | 1280 | 834 | allocation and collection |
| nbody | 344 | 513 | 513 | floats through objects |
| fannkuch | 1552 | 1501 | 1514 | array indexing |
| hashmap-u64 | 430 | 358 | 353 | |
| hashmap-string | 305 | 112 | 101 | |
| backend | 707 | 660 | 590 | json, middleware closures, string building |
| float-format | 80 | 55 | 51 | `${x}` vs strconv 'f' -1 |
| float-parse | 67 | 52 | 52 | std/float parseFloat vs strconv |
| httpd ping, 32 conns | 90k req/s | 136k req/s | 191k req/s | the load generator shares the cores |
| httpd orders, 32 conns | 62k req/s | 85k req/s | 139k req/s | |

## Collections in perf

The run time has USDT probes `bober:gc_start`, of the bytes allocated since the last collection, and `bober:gc_end`, of
those still live after it; the time between them is the pause. Each build moves them, so register them after it, as
root; the rest runs as a user if `kernel.perf_event_paranoid` is -1 and /sys/kernel/tracing is readable.

    sudo perf probe -q -d 'sdt_bober:*'; sudo perf probe -q -x build/<name> 'sdt_bober:*'
    perf stat -e 'sdt_bober:*' build/<name>
    perf record -e 'sdt_bober:*' build/<name> && perf script
