package main

import (
	"log"
	"net"
	"time"
)

const connReadTimeout = 15 * time.Second

type Server struct {
	reg   *Registry
	build string
	token string
}

func NewServer(build, token string) *Server {
	return &Server{reg: NewRegistry(), build: build, token: token}
}

func (s *Server) Run(tcpAddr string) error {
	ln, err := net.Listen("tcp", tcpAddr)
	if err != nil {
		return err
	}
	log.Printf("matchmaker listening tcp=%s build=%s", tcpAddr, s.build)
	return s.Serve(ln)
}

func (s *Server) Serve(ln net.Listener) error {
	go s.sweepLoop()
	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		go s.handleConn(conn)
	}
}

func (s *Server) sweepLoop() {
	t := time.NewTicker(5 * time.Second)
	defer t.Stop()
	for now := range t.C {
		for _, l := range s.reg.Sweep(now) {
			log.Printf("lobby %d expired (ttl)", l.ID)
			l.control.Close()
		}
	}
}

func (s *Server) handleConn(conn net.Conn) {
	conn.SetReadDeadline(time.Now().Add(connReadTimeout))
	op, body, err := readFrame(conn)
	if err != nil {
		conn.Close()
		return
	}
	switch op {
	case opList:
		s.handleList(conn, body)
	case opRegister:
		s.handleRegister(conn, body)
	default:
		conn.Close()
	}
}

func (s *Server) checkToken(tok string) bool {
	return s.token == "" || tok == s.token
}

func remoteIP(conn net.Conn) string {
	if ta, ok := conn.RemoteAddr().(*net.TCPAddr); ok {
		if v4 := ta.IP.To4(); v4 != nil {
			return v4.String()
		}
		return ta.IP.String()
	}
	host, _, _ := net.SplitHostPort(conn.RemoteAddr().String())
	return host
}
