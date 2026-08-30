package main

import (
	"encoding/binary"
	"errors"
	"io"
)

const (
	maxFrameLen  = 512
	maxStringLen = 64
)

const (
	opList       = 0x01
	opRegister   = 0x02
	opUpdate     = 0x04
	opPing       = 0x05
	opPong       = 0x06
	opEntry      = 0x81
	opListEnd    = 0x82
	opRegistered = 0x83
)

var (
	errFrameTooBig  = errors.New("matchmaker: frame too large")
	errStringTooBig = errors.New("matchmaker: string too large")
	errTruncated    = errors.New("matchmaker: truncated frame")
)

func readFrame(r io.Reader) (byte, []byte, error) {
	var lenbuf [4]byte
	if _, err := io.ReadFull(r, lenbuf[:]); err != nil {
		return 0, nil, err
	}
	n := binary.BigEndian.Uint32(lenbuf[:])
	if n == 0 || n > maxFrameLen {
		return 0, nil, errFrameTooBig
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return 0, nil, err
	}
	return buf[0], buf[1:], nil
}

func writeFrame(w io.Writer, op byte, body []byte) error {
	total := 1 + len(body)
	out := make([]byte, 4+total)
	binary.BigEndian.PutUint32(out[0:4], uint32(total))
	out[4] = op
	copy(out[5:], body)
	_, err := w.Write(out)
	return err
}

type bodyWriter struct{ b []byte }

func (bw *bodyWriter) u8(v byte)    { bw.b = append(bw.b, v) }
func (bw *bodyWriter) u16(v uint16) { bw.b = append(bw.b, byte(v>>8), byte(v)) }
func (bw *bodyWriter) u32(v uint32) {
	bw.b = append(bw.b, byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
}
func (bw *bodyWriter) str(s string) {
	if len(s) > maxStringLen {
		s = s[:maxStringLen]
	}
	bw.u16(uint16(len(s)))
	bw.b = append(bw.b, s...)
}

type bodyReader struct {
	b   []byte
	pos int
	err error
}

func (br *bodyReader) u8() byte {
	if br.err != nil || br.pos+1 > len(br.b) {
		br.err = errTruncated
		return 0
	}
	v := br.b[br.pos]
	br.pos++
	return v
}
func (br *bodyReader) u16() uint16 {
	if br.err != nil || br.pos+2 > len(br.b) {
		br.err = errTruncated
		return 0
	}
	v := binary.BigEndian.Uint16(br.b[br.pos:])
	br.pos += 2
	return v
}
func (br *bodyReader) u32() uint32 {
	if br.err != nil || br.pos+4 > len(br.b) {
		br.err = errTruncated
		return 0
	}
	v := binary.BigEndian.Uint32(br.b[br.pos:])
	br.pos += 4
	return v
}
func (br *bodyReader) str() string {
	n := int(br.u16())
	if br.err != nil {
		return ""
	}
	if n > maxStringLen || br.pos+n > len(br.b) {
		br.err = errStringTooBig
		return ""
	}
	s := string(br.b[br.pos : br.pos+n])
	br.pos += n
	return s
}
