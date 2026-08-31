package main

import (
	"io"
	"log"
	"net"
	"testing"
	"time"
)

func init() { log.SetOutput(io.Discard) }

func startTestServer(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go NewServer("1.4", "").Serve(ln)
	t.Cleanup(func() { ln.Close() })
	return ln.Addr().String()
}

func dial(t *testing.T, addr string) net.Conn {
	t.Helper()
	c, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	c.SetDeadline(time.Now().Add(5 * time.Second))
	return c
}

func registerHost(t *testing.T, addr, build, name string, max byte, bsnp uint32) (net.Conn, uint32) {
	t.Helper()
	hc := dial(t, addr)
	bw := &bodyWriter{}
	bw.str("")
	bw.str(build)
	bw.str(name)
	bw.u8(max)
	bw.u16(26041)
	bw.u16(26042)
	bw.u32(bsnp)
	if err := writeFrame(hc, opRegister, bw.b); err != nil {
		t.Fatal(err)
	}
	op, body, err := readFrame(hc)
	if err != nil || op != opRegistered {
		t.Fatalf("register reply op=%#x err=%v", op, err)
	}
	return hc, (&bodyReader{b: body}).u32()
}

func doList(t *testing.T, addr, build string) []*bodyReader {
	t.Helper()
	lc := dial(t, addr)
	defer lc.Close()
	bw := &bodyWriter{}
	bw.str("")
	bw.str(build)
	if err := writeFrame(lc, opList, bw.b); err != nil {
		t.Fatal(err)
	}
	var entries []*bodyReader
	for {
		op, body, err := readFrame(lc)
		if err != nil {
			t.Fatal(err)
		}
		if op == opListEnd {
			return entries
		}
		if op != opEntry {
			t.Fatalf("unexpected op %#x", op)
		}
		entries = append(entries, &bodyReader{b: body})
	}
}

func TestRegisterAndList(t *testing.T) {
	addr := startTestServer(t)
	hc, lobbyID := registerHost(t, addr, "1.4", "MYLOBBY", 4, 0xABCDEF01)
	defer hc.Close()

	entries := doList(t, addr, "1.4")
	if len(entries) != 1 {
		t.Fatalf("got %d entries", len(entries))
	}
	e := entries[0]
	if id := e.u32(); id != lobbyID {
		t.Fatalf("id %d != %d", id, lobbyID)
	}
	if ip := e.str(); ip != "127.0.0.1" {
		t.Fatalf("ip %q", ip)
	}
	if lp := e.u16(); lp != 26041 {
		t.Fatalf("lobbyPort %d", lp)
	}
	e.u16()
	if s := e.u32(); s != 0xABCDEF01 {
		t.Fatalf("bsnp %#x", s)
	}
	if name := e.str(); name != "MYLOBBY" {
		t.Fatalf("name %q", name)
	}
	if p := e.u8(); p != 1 {
		t.Fatalf("players %d", p)
	}
}

func TestUpdatePlayerCount(t *testing.T) {
	addr := startTestServer(t)
	hc, _ := registerHost(t, addr, "1.4", "H", 4, 0)
	defer hc.Close()

	bw := &bodyWriter{}
	bw.u8(2)
	bw.u8(1)
	if err := writeFrame(hc, opUpdate, bw.b); err != nil {
		t.Fatal(err)
	}
	time.Sleep(50 * time.Millisecond)

	e := doList(t, addr, "1.4")[0]
	e.u32()
	e.str()
	e.u16()
	e.u16()
	e.u32()
	e.str()
	if p := e.u8(); p != 2 {
		t.Fatalf("players %d", p)
	}
	e.u8()
	if s := e.u8(); s != 1 {
		t.Fatalf("status %d", s)
	}
}

func TestPingPong(t *testing.T) {
	addr := startTestServer(t)
	hc, _ := registerHost(t, addr, "1.4", "H", 4, 0)
	defer hc.Close()

	if err := writeFrame(hc, opPing, nil); err != nil {
		t.Fatal(err)
	}
	op, _, err := readFrame(hc)
	if err != nil || op != opPong {
		t.Fatalf("op=%#x err=%v", op, err)
	}
}

func TestRegisterBuildMismatch(t *testing.T) {
	addr := startTestServer(t)
	hc := dial(t, addr)
	defer hc.Close()
	bw := &bodyWriter{}
	bw.str("")
	bw.str("9.9")
	bw.str("H")
	bw.u8(4)
	bw.u16(26041)
	bw.u16(26042)
	bw.u32(0)
	writeFrame(hc, opRegister, bw.b)
	if _, _, err := readFrame(hc); err == nil {
		t.Fatal("expected closed connection on build mismatch")
	}
}

func TestListBuildFilter(t *testing.T) {
	addr := startTestServer(t)
	hc, _ := registerHost(t, addr, "1.4", "H", 4, 0)
	defer hc.Close()
	if n := len(doList(t, addr, "9.9")); n != 0 {
		t.Fatalf("expected 0 entries for other build, got %d", n)
	}
	if n := len(doList(t, addr, "1.4")); n != 1 {
		t.Fatalf("expected 1, got %d", n)
	}
}

func TestControlDropRemovesLobby(t *testing.T) {
	addr := startTestServer(t)
	hc, _ := registerHost(t, addr, "1.4", "H", 4, 0)
	hc.Close()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if len(doList(t, addr, "1.4")) == 0 {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatal("lobby not removed after control channel drop")
}
