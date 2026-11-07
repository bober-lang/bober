package main

import (
	"bench/util"
	"encoding/json"
	"strconv"
	"strings"
	"time"
)

type Item struct {
	Sku   string  `json:"sku"`
	Qty   int32   `json:"qty"`
	Price float64 `json:"price"`
}

type Order struct {
	Id    int64  `json:"id"`
	User  string `json:"user"`
	Items []Item `json:"items"`
	Note  string `json:"note"`
}

type Reply struct {
	Id    int64   `json:"id"`
	User  string  `json:"user"`
	Count int32   `json:"count"`
	Total float64 `json:"total"`
	Ok    bool    `json:"ok"`
}

type Ctx struct {
	method, path, auth, body string
	status                   int
	out                      string
	user                     string
}

type Handler func(c *Ctx) error
type Middleware func(c *Ctx, next Handler) error

func chain(mws []Middleware, h Handler) Handler {
	next := h
	for i := range mws {
		mw := mws[len(mws)-1-i]
		inner := next
		next = func(c *Ctx) error { return mw(c, inner) }
	}
	return next
}

var logged int64

func auth(c *Ctx, next Handler) error {
	if !strings.HasPrefix(c.auth, "Bearer ") {
		c.status = 401
		c.out = `{"error":"unauthorized"}`
		return nil
	}
	c.user = c.auth[7:]
	return next(c)
}

func logging(c *Ctx, next Handler) error {
	err := next(c)
	if err == nil {
		logged += int64(len(c.out) + len(c.path))
	}
	return err
}

func recover_(c *Ctx, next Handler) error {
	if err := next(c); err != nil {
		c.status = 400
		c.out = `{"error":"bad json"}`
	}
	return nil
}

func orders(c *Ctx) error {
	segs := strings.Split(c.path, "/")
	if len(segs) != 4 || segs[1] != "api" || segs[2] != "orders" || c.method != "POST" {
		c.status = 404
		c.out = `{"error":"not found"}`
		return nil
	}
	var o Order
	if err := json.Unmarshal([]byte(c.body), &o); err != nil {
		return err
	}
	r := Reply{Id: o.Id, User: o.User, Count: int32(len(o.Items)), Ok: true}
	for _, it := range o.Items {
		r.Total += float64(it.Qty) * it.Price
	}
	r.Ok = strconv.FormatInt(o.Id, 10) == segs[3] && o.User == c.user
	out, err := json.Marshal(&r)
	if err != nil {
		return err
	}
	c.out = string(out)
	return nil
}

func respond(c *Ctx) string {
	reason := "Error"
	if c.status == 200 {
		reason = "OK"
	} else if c.status == 404 {
		reason = "Not Found"
	}
	return "HTTP/1.1 " + strconv.Itoa(c.status) + " " + reason + "\r\nContent-Type: application/json\r\nContent-Length: " +
		strconv.Itoa(len(c.out)) + "\r\nX-Request-Id: " + strconv.FormatInt(logged, 10) + "\r\nConnection: keep-alive\r\n\r\n" + c.out
}

func main() {
	n := int(util.Arg(0, 200_000))
	h := chain([]Middleware{recover_, logging, auth}, orders)
	t0 := time.Now()
	var bytes, cents int64
	for i := 0; i < n; i++ {
		var items []string
		for j := 1; j <= 1+i%5; j++ {
			items = append(items, `{"sku":"SKU-`+strconv.Itoa(i%1000)+"-"+strconv.Itoa(j)+`","qty":`+strconv.Itoa(j)+`,"price":`+
				strconv.FormatFloat(float64(i%97)+0.5, 'g', -1, 64)+"}")
		}
		body := `{"id":` + strconv.Itoa(i) + `,"user":"user` + strconv.Itoa(i%100) + `","items":[` + strings.Join(items, ",") +
			`],"note":"order number ` + strconv.Itoa(i) + ` from a loyal customer"}`
		c := &Ctx{method: "POST", path: "/api/orders/" + strconv.Itoa(i), auth: "Bearer user" + strconv.Itoa(i%100), body: body, status: 200}
		if i%50 == 0 {
			c.auth = "nope"
		} else if i%50 == 1 {
			c.path = "/api/missing"
		} else if i%50 == 2 {
			c.body = `{"id": oops}`
		}
		h(c)
		res := respond(c)
		bytes += int64(len(res))
		if c.status == 200 {
			var r Reply
			json.Unmarshal([]byte(c.out), &r)
			cents += int64(r.Total * 100)
		}
	}
	util.Report("backend", t0, bytes+cents+logged)
}
