// A keep-alive HTTP load generator: c connections for d seconds; prints requests/s and latencies.
package main

import (
	"flag"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const body = `{"id":4711,"user":"user42","items":[{"sku":"SKU-1","qty":1,"price":9.5},{"sku":"SKU-2","qty":2,"price":19.5},{"sku":"SKU-3","qty":3,"price":29.5}],"note":"order number 4711 from a loyal customer"}`

func main() {
	addr := flag.String("addr", "http://127.0.0.1:8081", "server")
	c := flag.Int("c", 32, "connections")
	d := flag.Duration("d", 3*time.Second, "duration")
	mode := flag.String("mode", "ping", "ping or orders")
	flag.Parse()
	client := &http.Client{Transport: &http.Transport{MaxIdleConnsPerHost: *c, MaxConnsPerHost: *c, DisableCompression: true}}
	var ok, bad atomic.Int64
	lats := make([][]int64, *c)
	var wg sync.WaitGroup
	deadline := time.Now().Add(*d)
	for i := 0; i < *c; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for time.Now().Before(deadline) {
				t := time.Now()
				var res *http.Response
				var err error
				if *mode == "ping" {
					res, err = client.Get(*addr + "/ping")
				} else {
					res, err = client.Post(*addr+"/api/orders/4711", "application/json", strings.NewReader(body))
				}
				if err == nil {
					_, err = io.Copy(io.Discard, res.Body)
					res.Body.Close()
				}
				if err != nil || res.StatusCode != 200 {
					bad.Add(1)
					continue
				}
				ok.Add(1)
				lats[i] = append(lats[i], int64(time.Since(t)/time.Microsecond))
			}
		}(i)
	}
	wg.Wait()
	var all []int64
	for _, l := range lats {
		all = append(all, l...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
	p := func(q float64) int64 {
		if len(all) == 0 {
			return 0
		}
		return all[int(float64(len(all)-1)*q)]
	}
	fmt.Printf("%s c=%d: %.0f req/s, %d errors, p50 %d us, p99 %d us\n", *mode, *c, float64(ok.Load())/d.Seconds(), bad.Load(), p(0.5), p(0.99))
}
