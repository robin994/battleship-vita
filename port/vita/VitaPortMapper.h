#pragma once

#include <cstdint>
#include <string>

namespace ssb64::netplay::platform {

enum class PortMapState : int {
    Idle = 0,
    Working,
    Mapped,
    Failed,
};

// Best-effort UPnP-IGD automatic port forwarding. RequestPortMapping starts a
// background worker that discovers the gateway, queries the external IP and
// adds TCP/UDP mappings; it never blocks the caller. ReleasePortMapping tears
// the mappings down and joins the worker. All calls are made from
// NetworkManager's worker thread.
void RequestPortMapping(uint16_t tcpPort, uint16_t udpPort, const std::string& localIp);
PortMapState GetPortMapState();
std::string GetMappedExternalIp();
void ReleasePortMapping();

} // namespace ssb64::netplay::platform
