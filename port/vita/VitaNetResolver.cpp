#include "VitaNetResolver.h"

#include <cstring>

#ifdef __vita__
#include <arpa/inet.h>
#include <psp2/net/net.h>

extern "C" void port_log(const char* fmt, ...);

namespace ssb64::netplay {

bool ResolveHostV4(const std::string& host, uint32_t& outIpv4NetOrder) {
    if (host.empty()) return false;

    in_addr numeric{};
    if (inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
        outIpv4NetOrder = numeric.s_addr;
        return true;
    }

    const int rid = sceNetResolverCreate("bsnp_resolver", nullptr, 0);
    if (rid < 0) {
        port_log("[NETPLAY] resolver create failed 0x%08X\n", static_cast<unsigned>(rid));
        return false;
    }

    SceNetInAddr addr{};
    const int r = sceNetResolverStartNtoa(rid, host.c_str(), &addr, 5 * 1000 * 1000, 2, 0);
    sceNetResolverDestroy(rid);

    if (r < 0) {
        port_log("[NETPLAY] resolve %s failed 0x%08X\n", host.c_str(), static_cast<unsigned>(r));
        return false;
    }
    outIpv4NetOrder = addr.s_addr;
    return true;
}

} // namespace ssb64::netplay

#else // desktop

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace ssb64::netplay {

bool ResolveHostV4(const std::string& host, uint32_t& outIpv4NetOrder) {
    if (host.empty()) return false;

    in_addr numeric{};
    if (inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
        outIpv4NetOrder = numeric.s_addr;
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        return false;
    }

    bool ok = false;
    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET && it->ai_addr != nullptr) {
            sockaddr_in sa{};
            std::memcpy(&sa, it->ai_addr, sizeof(sa));
            outIpv4NetOrder = sa.sin_addr.s_addr;
            ok = true;
            break;
        }
    }
    freeaddrinfo(res);
    return ok;
}

} // namespace ssb64::netplay

#endif
