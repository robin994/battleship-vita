package main

import (
	"log"
	"net"
	"time"
)

func (s *Server) handleRegister(conn net.Conn, body []byte) {
	br := &bodyReader{b: body}
	token := br.str()
	build := br.str()
	hostName := br.str()
	maxPlayers := br.u8()
	lobbyPort := br.u16()
	gameplayPort := br.u16()
	bsnpSession := br.u32()
	if br.err != nil || !s.checkToken(token) || build != s.build {
		conn.Close()
		return
	}
	if maxPlayers < 2 || maxPlayers > 4 {
		maxPlayers = 4
	}
	if lobbyPort == 0 || gameplayPort == 0 {
		conn.Close()
		return
	}

	lobby := &Lobby{
		Build:        build,
		HostName:     hostName,
		PublicIP:     remoteIP(conn),
		LobbyPort:    lobbyPort,
		GameplayPort: gameplayPort,
		BsnpSession:  bsnpSession,
		Max:          maxPlayers,
		control:      conn,
	}
	if !s.reg.Register(lobby) {
		conn.Close()
		return
	}
	bw := &bodyWriter{}
	bw.u32(lobby.ID)
	if lobby.writeControl(opRegistered, bw.b) != nil {
		s.reg.Remove(lobby.ID)
		conn.Close()
		return
	}
	log.Printf("lobby %d registered host=%q ip=%s ports=%d/%d build=%s",
		lobby.ID, hostName, lobby.PublicIP, lobbyPort, gameplayPort, build)

	s.controlLoop(lobby)

	s.reg.Remove(lobby.ID)
	conn.Close()
	log.Printf("lobby %d closed (control channel down)", lobby.ID)
}

func (s *Server) controlLoop(lobby *Lobby) {
	for {
		lobby.control.SetReadDeadline(time.Now().Add(lobbyTTL))
		op, body, err := readFrame(lobby.control)
		if err != nil {
			return
		}
		switch op {
		case opPing:
			lobby.touch()
			if lobby.writeControl(opPong, nil) != nil {
				return
			}
		case opUpdate:
			br := &bodyReader{b: body}
			players := br.u8()
			status := br.u8()
			if br.err == nil {
				lobby.update(players, status)
			}
		default:
		}
	}
}
