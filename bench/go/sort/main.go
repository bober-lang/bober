package main

import (
	"bench/util"
	"cmp"
	"slices"
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

func quicksort(xs []int32, lo, hi int32) {
	for hi-lo > 16 {
		p := xs[(lo+hi)/2]
		i, j := lo, hi
		for i <= j {
			for xs[i] < p {
				i++
			}
			for xs[j] > p {
				j--
			}
			if i <= j {
				xs[i], xs[j] = xs[j], xs[i]
				i++
				j--
			}
		}
		if j-lo < hi-i {
			quicksort(xs, lo, j)
			lo = i
		} else {
			quicksort(xs, i, hi)
			hi = j
		}
	}
	for i := lo + 1; i <= hi; i++ {
		v := xs[i]
		j := i - 1
		for j >= lo && xs[j] > v {
			xs[j+1] = xs[j]
			j--
		}
		xs[j+1] = v
	}
}

type Item struct {
	key    int64
	name   string
	weight float64
}

func mergeSort(xs []*Item, before func(a, b *Item) bool) []*Item {
	if len(xs) < 2 {
		return xs
	}
	mid := len(xs) / 2
	a := mergeSort(append([]*Item(nil), xs[:mid]...), before)
	b := mergeSort(append([]*Item(nil), xs[mid:]...), before)
	var out []*Item
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
	xs := make([]int32, n)
	for i := range xs {
		xs[i] = int32(r.next() >> 33)
	}
	t0 := time.Now()
	quicksort(xs, 0, n-1)
	var check int64
	for _, x := range xs {
		check = check*31 + int64(x)
	}
	util.Report("sort-quick-i32", t0, check)

	m := n / 5
	items := make([]*Item, 0, m)
	for i := int32(0); i < m; i++ {
		k := r.next() >> 40
		items = append(items, &Item{int64(k), "item" + strconv.FormatUint(k, 10), float64(k) / 3.0})
	}
	t0 = time.Now()
	sorted := mergeSort(items, func(a, b *Item) bool { return a.key < b.key })
	check = 0
	for _, it := range sorted {
		check = check*31 + it.key
	}
	util.Report("sort-merge-objects", t0, check)

	t0 = time.Now()
	slices.SortStableFunc(items, func(a, b *Item) int { return cmp.Compare(a.key, b.key) })
	check = 0
	for _, it := range items {
		check = check*31 + it.key
	}
	util.Report("sort-std-objects", t0, check)
}
