package util

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

// Arg is argument i as an integer, or d if absent or malformed.
func Arg(i int, d int64) int64 {
	if len(os.Args) <= i+1 {
		return d
	}
	v, err := strconv.ParseInt(os.Args[i+1], 10, 64)
	if err != nil {
		return d
	}
	return v
}

// Report prints a benchmark line: name, ms, and a checksum.
func Report(name string, t0 time.Time, check int64) {
	fmt.Printf("%s %d ms check=%d\n", name, time.Since(t0).Milliseconds(), check)
}
