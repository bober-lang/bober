package main

import (
	"bench/util"
	"strconv"
	"time"
)

type rng struct{ s uint64 }

func (r *rng) next() uint64 {
	r.s ^= r.s << 13
	r.s ^= r.s >> 7
	r.s ^= r.s << 17
	return r.s
}

// Item held by value, padded to the 40 bytes of Bober's (a string is 24 there), as Bober's release build holds a class
// never written after its constructor.
type Item struct {
	key    int64
	name   string
	weight float64
	_      [8]byte
}

func mergeSort(xs []Item, before func(a, b Item) bool) []Item {
	if len(xs) < 2 {
		return xs
	}
	mid := len(xs) / 2
	a := mergeSort(append([]Item(nil), xs[:mid]...), before)
	b := mergeSort(append([]Item(nil), xs[mid:]...), before)
	var out []Item
	i, j := 0, 0
	for k := 0; k < len(xs); k++ {
		if j == len(b) || i < len(a) && !before(b[j], a[i]) {
			out = append(out, a[i])
			i++
		} else {
			out = append(out, b[j])
			j++
		}
	}
	return out
}

func main() {
	n := int32(util.Arg(0, 5_000_000))
	r := &rng{42 | 1}
	for i := int32(0); i < n; i++ {
		r.next()
	}
	m := n / 5
	items := make([]Item, 0, m)
	for i := int32(0); i < m; i++ {
		k := r.next() >> 40
		items = append(items, Item{key: int64(k), name: "item" + strconv.FormatUint(k, 10), weight: float64(k) / 3.0})
	}
	t0 := time.Now()
	sorted := mergeSort(items, func(a, b Item) bool { return a.key < b.key })
	var check int64
	for _, it := range sorted {
		check = check*31 + it.key
	}
	util.Report("sort-merge-values", t0, check)
}
