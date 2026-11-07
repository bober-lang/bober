package main

import (
	"bench/util"
	"strconv"
	"time"
)

func main() {
	n := int(util.Arg(0, 1_000_000))
	x := uint64(88172645463325252)
	xs := make([]float64, n)
	for i := range xs {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		xs[i] = float64(x>>20)/1024.0 + 0.5
	}
	t0 := time.Now()
	ss := make([]string, n)
	var l int64
	for i := range xs {
		ss[i] = strconv.FormatFloat(xs[i], 'f', -1, 64)
		l += int64(len(ss[i]))
	}
	util.Report("float-format", t0, l)
	t0 = time.Now()
	sum := 0.0
	for i := range ss {
		v, err := strconv.ParseFloat(ss[i], 64)
		if err != nil {
			return
		}
		sum += v
	}
	util.Report("float-parse", t0, int64(sum))
}
