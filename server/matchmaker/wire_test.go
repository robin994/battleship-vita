package main

import (
	"bytes"
	"testing"
)

func TestFrameRoundTrip(t *testing.T) {
	var buf bytes.Buffer
	bw := &bodyWriter{}
	bw.str("hello")
	bw.u32(0xDEADBEEF)
	bw.u8(3)
	if err := writeFrame(&buf, opRegister, bw.b); err != nil {
		t.Fatal(err)
	}
	op, body, err := readFrame(&buf)
	if err != nil {
		t.Fatal(err)
	}
	if op != opRegister {
		t.Fatalf("op = %#x", op)
	}
	br := &bodyReader{b: body}
	if s := br.str(); s != "hello" {
		t.Fatalf("str = %q", s)
	}
	if v := br.u32(); v != 0xDEADBEEF {
		t.Fatalf("u32 = %#x", v)
	}
	if v := br.u8(); v != 3 {
		t.Fatalf("u8 = %d", v)
	}
	if br.err != nil {
		t.Fatal(br.err)
	}
}

func TestFrameRejectsOversize(t *testing.T) {
	buf := bytes.NewBuffer([]byte{0x00, 0x00, 0x10, 0x00})
	if _, _, err := readFrame(buf); err != errFrameTooBig {
		t.Fatalf("err = %v", err)
	}
}

func TestBodyReaderTruncation(t *testing.T) {
	br := &bodyReader{b: []byte{0x01}}
	br.u32()
	if br.err != errTruncated {
		t.Fatalf("err = %v", br.err)
	}
}

func TestStringCapped(t *testing.T) {
	long := make([]byte, maxStringLen+10)
	bw := &bodyWriter{}
	bw.str(string(long))
	br := &bodyReader{b: bw.b}
	if got := br.str(); len(got) != maxStringLen {
		t.Fatalf("len = %d", len(got))
	}
}
