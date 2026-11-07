package main

import (
	"bench/util"
	"time"
)

func hanoi(n, from, to, via int32) int64 {
	if n == 0 {
		return 0
	}
	return hanoi(n-1, from, via, to) + 1 + hanoi(n-1, via, to, from)
}

type peg struct{ xs []int32 }

func (p *peg) push(x int32) { p.xs = append(p.xs, x) }
func (p *peg) pop() int32 {
	x := p.xs[len(p.xs)-1]
	p.xs = p.xs[:len(p.xs)-1]
	return x
}

func moves(n int32, from, to, via *peg) int64 {
	if n == 0 {
		return 0
	}
	a := moves(n-1, from, via, to)
	to.push(from.pop())
	return a + 1 + moves(n-1, via, to, from)
}

func main() {
	n := int32(util.Arg(0, 27))
	t0 := time.Now()
	util.Report("hanoi-count", t0, hanoi(n, 0, 2, 1))
	m := n - 3
	pegs := []*peg{{}, {}, {}}
	for i := int32(0); i < m; i++ {
		pegs[0].push(m - i)
	}
	t0 = time.Now()
	c := moves(m, pegs[0], pegs[2], pegs[1])
	util.Report("hanoi-stacks", t0, c+int64(len(pegs[2].xs)))
}
