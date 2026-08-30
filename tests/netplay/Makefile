CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
TARGET := $(ROOT)/build/netplay_loopback_test
SOURCES := \
	$(ROOT)/tests/netplay/netplay_loopback_test.cpp \
	$(ROOT)/port/netplay/NetplayProtocol.cpp \
	$(ROOT)/port/netplay/GameplaySession.cpp \
	$(ROOT)/port/netplay/LanDiscovery.cpp \
	$(ROOT)/port/netplay/LobbySession.cpp \
	$(ROOT)/port/netplay/RendezvousClient.cpp \
	$(ROOT)/port/vita/VitaNetResolver.cpp \
	$(ROOT)/port/vita/VitaNetworkTransport.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(ROOT)/build
	$(CXX) $(CXXFLAGS) -I$(ROOT) -idirafter $(ROOT)/decomp/include -idirafter $(ROOT)/decomp/src $(SOURCES) -o $@

run: $(TARGET)
	$(TARGET)

clean:
	rm -f $(TARGET)
