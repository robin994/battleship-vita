package main

import "net"

func (s *Server) handleList(conn net.Conn, body []byte) {
	defer conn.Close()
	br := &bodyReader{b: body}
	token := br.str()
	build := br.str()
	if br.err != nil || !s.checkToken(token) {
		return
	}

	for _, snap := range s.reg.List() {
		if build != "" && snap.Build != build {
			continue
		}
		bw := &bodyWriter{}
		bw.u32(snap.ID)
		bw.str(snap.PublicIP)
		bw.u16(snap.LobbyPort)
		bw.u16(snap.GameplayPort)
		bw.u32(snap.BsnpSession)
		bw.str(snap.HostName)
		bw.u8(snap.Players)
		bw.u8(snap.Max)
		bw.u8(snap.Status)
		bw.str(snap.Build)
		if writeFrame(conn, opEntry, bw.b) != nil {
			return
		}
	}
	writeFrame(conn, opListEnd, nil)
}
