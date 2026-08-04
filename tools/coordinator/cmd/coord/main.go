package main

import (
	"flag"
	"log"

	"github.com/GeneralsZulu/LegacyGameClient/tools/coordinator"
)

func main() {
	tcp := flag.String("tcp", ":27500", "TCP signaling listen address")
	udp := flag.String("udp", ":27501", "UDP STUN listen address")
	flag.Parse()

	srv := coordinator.NewServer()
	if err := srv.Run(*tcp, *udp); err != nil {
		log.Fatalf("server: %v", err)
	}
}
