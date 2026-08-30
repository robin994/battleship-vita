package main

import (
	"math/rand"
	"net"
	"sync"
	"time"
)

const (
	lobbyTTL   = 20 * time.Second
	maxLobbies = 128
)

type Lobby struct {
	ID           uint32
	Build        string
	HostName     string
	PublicIP     string
	LobbyPort    uint16
	GameplayPort uint16
	BsnpSession  uint32
	Max          uint8

	mu       sync.Mutex
	control  net.Conn
	writeMu  sync.Mutex
	players  uint8
	status   uint8
	lastSeen time.Time
}

func (l *Lobby) touch() {
	l.mu.Lock()
	l.lastSeen = time.Now()
	l.mu.Unlock()
}

func (l *Lobby) update(players, status uint8) {
	l.mu.Lock()
	l.players = players
	l.status = status
	l.lastSeen = time.Now()
	l.mu.Unlock()
}

func (l *Lobby) writeControl(op byte, body []byte) error {
	l.writeMu.Lock()
	defer l.writeMu.Unlock()
	return writeFrame(l.control, op, body)
}

func (l *Lobby) snapshot() LobbySnapshot {
	l.mu.Lock()
	defer l.mu.Unlock()
	return LobbySnapshot{
		ID:           l.ID,
		Build:        l.Build,
		HostName:     l.HostName,
		PublicIP:     l.PublicIP,
		LobbyPort:    l.LobbyPort,
		GameplayPort: l.GameplayPort,
		BsnpSession:  l.BsnpSession,
		Players:      l.players,
		Max:          l.Max,
		Status:       l.status,
	}
}

type Registry struct {
	mu     sync.Mutex
	byID   map[uint32]*Lobby
	nextID uint32
}

func NewRegistry() *Registry {
	return &Registry{byID: make(map[uint32]*Lobby), nextID: rand.Uint32() | 1}
}

func (r *Registry) Register(l *Lobby) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.byID) >= maxLobbies {
		return false
	}
	id := r.nextID
	r.nextID++
	if r.nextID == 0 {
		r.nextID = 1
	}
	l.ID = id
	l.players = 1
	l.status = 0
	l.lastSeen = time.Now()
	r.byID[id] = l
	return true
}

func (r *Registry) Remove(id uint32) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.byID, id)
}

type LobbySnapshot struct {
	ID           uint32
	Build        string
	HostName     string
	PublicIP     string
	LobbyPort    uint16
	GameplayPort uint16
	BsnpSession  uint32
	Players      uint8
	Max          uint8
	Status       uint8
}

func (r *Registry) List() []LobbySnapshot {
	r.mu.Lock()
	lobbies := make([]*Lobby, 0, len(r.byID))
	for _, l := range r.byID {
		lobbies = append(lobbies, l)
	}
	r.mu.Unlock()

	out := make([]LobbySnapshot, 0, len(lobbies))
	for _, l := range lobbies {
		out = append(out, l.snapshot())
	}
	return out
}

func (r *Registry) Sweep(now time.Time) []*Lobby {
	r.mu.Lock()
	defer r.mu.Unlock()
	var dead []*Lobby
	for id, l := range r.byID {
		l.mu.Lock()
		expired := now.Sub(l.lastSeen) > lobbyTTL
		l.mu.Unlock()
		if expired {
			dead = append(dead, l)
			delete(r.byID, id)
		}
	}
	return dead
}
