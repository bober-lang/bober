package main

import (
	"bench/util"
	"time"
)

func fannkuch(n int32) (int32, int32) {
	perm := make([]int32, n)
	perm1 := make([]int32, n)
	count := make([]int32, n)
	for i := range perm1 {
		perm1[i] = int32(i)
	}
	var maxFlips, checksum, permCount int32
	r := n
	for {
		for r != 1 {
			count[r-1] = r
			r--
		}
		copy(perm, perm1)
		var flips int32
		k := perm[0]
		for k != 0 {
			i, j := int32(0), k
			for i < j {
				perm[i], perm[j] = perm[j], perm[i]
				i++
				j--
			}
			flips++
			k = perm[0]
		}
		if flips > maxFlips {
			maxFlips = flips
		}
		if permCount%2 == 0 {
			checksum += flips
		} else {
			checksum -= flips
		}
		for {
			if r == n {
				return checksum, maxFlips
			}
			p0 := perm1[0]
			for i := int32(0); i < r; i++ {
				perm1[i] = perm1[i+1]
			}
			perm1[r] = p0
			count[r]--
			if count[r] > 0 {
				break
			}
			r++
		}
		permCount++
	}
}

func main() {
	n := int32(util.Arg(0, 11))
	t0 := time.Now()
	checksum, maxFlips := fannkuch(n)
	util.Report("fannkuch", t0, int64(checksum)*1000+int64(maxFlips))
}
