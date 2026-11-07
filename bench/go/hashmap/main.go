package main

import (
	"bench/util"
	"strconv"
	"time"
)

func main() {
	n := int32(util.Arg(0, 2_000_000))
	t0 := time.Now()
	m := map[uint64]uint64{}
	x := uint64(88172645463325252)
	for i := int32(0); i < n; i++ {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		m[x] = uint64(i)
	}
	var check uint64
	x = 88172645463325252
	for i := int32(0); i < 5*n; i++ {
		x ^= x << 13
		x ^= x >> 7
		x ^= x << 17
		if v, ok := m[x]; ok {
			check += v
		}
		if i%7 == 3 {
			x = 88172645463325252 + uint64(i)
		}
	}
	util.Report("hashmap-u64", t0, int64(check))

	k := n / 4
	keys := make([]string, k)
	for i := range keys {
		keys[i] = "user:" + strconv.Itoa(i*7919%int(k)) + ":session"
	}
	t0 = time.Now()
	sm := map[string]int32{}
	for _, key := range keys {
		if c, ok := sm[key]; ok {
			sm[key] = c + 1
		} else {
			sm[key] = 1
		}
	}
	var found int64
	for r := 0; r < 4; r++ {
		for _, key := range keys {
			if c, ok := sm[key]; ok {
				found += int64(c)
			}
		}
	}
	util.Report("hashmap-string", t0, found+int64(len(sm)))
}
