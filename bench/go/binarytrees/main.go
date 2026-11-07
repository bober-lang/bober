package main

import (
	"bench/util"
	"time"
)

type Node struct{ left, right *Node }

func make_(depth int32) *Node {
	if depth == 0 {
		return &Node{}
	}
	return &Node{make_(depth - 1), make_(depth - 1)}
}

func check(n *Node) int32 {
	if n.left == nil {
		return 1
	}
	return 1 + check(n.left) + check(n.right)
}

func main() {
	n := int32(util.Arg(0, 18))
	t0 := time.Now()
	min := int32(4)
	max := n
	if max < min+2 {
		max = min + 2
	}
	stretch := make_(max + 1)
	sum := int64(check(stretch))
	long := make_(max)
	for d := min; d <= max; d += 2 {
		iters := int32(1) << (max - d + min)
		var c int32
		for i := int32(1); i <= iters; i++ {
			c += check(make_(d))
		}
		sum += int64(c)
	}
	sum += int64(check(long))
	util.Report("binarytrees", t0, sum)
}
