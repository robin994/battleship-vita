package main

import (
	"testing"
	"time"
)

func mkLobby(build, name string, max uint8) *Lobby {
	return &Lobby{Build: build, HostName: name, PublicIP: "1.2.3.4",
		LobbyPort: 26041, GameplayPort: 26042, Max: max}
}

func TestRegistryRegisterListRemove(t *testing.T) {
	r := NewRegistry()
	l := mkLobby("1.4", "HOST", 4)
	if !r.Register(l) || l.ID == 0 {
		t.Fatalf("register failed id=%d", l.ID)
	}
	list := r.List()
	if len(list) != 1 || list[0].ID != l.ID || list[0].Players != 1 || list[0].PublicIP != "1.2.3.4" {
		t.Fatalf("list = %+v", list)
	}
	r.Remove(l.ID)
	if len(r.List()) != 0 {
		t.Fatal("still present after remove")
	}
}

func TestRegistryUpdate(t *testing.T) {
	r := NewRegistry()
	l := mkLobby("1.4", "H", 4)
	r.Register(l)
	l.update(3, 1)
	list := r.List()
	if list[0].Players != 3 || list[0].Status != 1 {
		t.Fatalf("list = %+v", list)
	}
}

func TestRegistrySweepTTL(t *testing.T) {
	r := NewRegistry()
	l := mkLobby("1.4", "H", 4)
	r.Register(l)
	l.mu.Lock()
	l.lastSeen = time.Now().Add(-2 * lobbyTTL)
	l.mu.Unlock()
	dead := r.Sweep(time.Now())
	if len(dead) != 1 || dead[0].ID != l.ID || len(r.List()) != 0 {
		t.Fatalf("sweep failed dead=%d list=%d", len(dead), len(r.List()))
	}
}

func TestRegistryMaxLobbies(t *testing.T) {
	r := NewRegistry()
	for i := 0; i < maxLobbies; i++ {
		if !r.Register(mkLobby("1.4", "H", 4)) {
			t.Fatalf("register %d failed", i)
		}
	}
	if r.Register(mkLobby("1.4", "H", 4)) {
		t.Fatal("expected cap rejection")
	}
}
