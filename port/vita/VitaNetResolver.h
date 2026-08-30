#pragma once

#include <cstdint>
#include <string>

namespace ssb64::netplay {

// Resolves `host` (a dotted-quad IPv4 or a DNS hostname) to an IPv4 address in
// network byte order (matching transport::SocketAddress.ipv4). Blocking on the
// caller's thread; keep it to the netplay worker. Returns false on failure.
bool ResolveHostV4(const std::string& host, uint32_t& outIpv4NetOrder);

} // namespace ssb64::netplay
