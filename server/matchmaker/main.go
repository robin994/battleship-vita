package main

import (
	"flag"
	"log"
	"math/rand"
	"time"
)

func main() {
	tcpAddr := flag.String("tcp", ":26050", "TCP listen address")
	build := flag.String("build", "1.3", "required client build id")
	token := flag.String("token", "", "optional shared token; empty = no token check")
	flag.Parse()

	rand.Seed(time.Now().UnixNano())
	log.Fatal(NewServer(*build, *token).Run(*tcpAddr))
}
