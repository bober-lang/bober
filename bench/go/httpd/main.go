package main

import (
	"bench/util"
	"encoding/json"
	"net/http"
	"strconv"
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

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /ping", func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte("pong"))
	})
	mux.HandleFunc("POST /api/orders/{id}", func(w http.ResponseWriter, r *http.Request) {
		var o Order
		if err := json.NewDecoder(r.Body).Decode(&o); err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		reply := Reply{Id: o.Id, User: o.User, Count: int32(len(o.Items))}
		for _, it := range o.Items {
			reply.Total += float64(it.Qty) * it.Price
		}
		reply.Ok = strconv.FormatInt(o.Id, 10) == r.PathValue("id")
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(&reply)
	})
	http.ListenAndServe(":"+strconv.FormatInt(util.Arg(0, 8082), 10), mux)
}
