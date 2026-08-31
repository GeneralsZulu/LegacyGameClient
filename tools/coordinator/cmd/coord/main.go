package main

import (
	"encoding/json"
	"flag"
	"log"
	"net/http"

	"github.com/GeneralsZulu/LegacyGameClient/tools/coordinator"
)

func main() {
	tcp := flag.String("tcp", ":27500", "TCP signaling listen address")
	udp := flag.String("udp", ":27501", "UDP STUN listen address")
	udp2 := flag.String("udp2", ":27503", "second STUN listen address for NAT self-classification (empty = disabled)")
	status := flag.String("status", "", "optional HTTP status listen address (e.g. :27502); serves JSON at /status")
	flag.Parse()

	srv := coordinator.NewServer()
	if *status != "" {
		mux := http.NewServeMux()
		mux.HandleFunc("/status", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(srv.Status())
		})
		go func() {
			if err := http.ListenAndServe(*status, mux); err != nil {
				log.Printf("status listener: %v", err)
			}
		}()
		log.Printf("HTTP status on %s/status", *status)
	}
	if err := srv.RunWithAltSTUN(*tcp, *udp, *udp2); err != nil {
		log.Fatalf("server: %v", err)
	}
}
