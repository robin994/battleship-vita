#include "VitaPortMapper.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" void port_log(const char* fmt, ...);

#ifdef __vita__
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace ssb64::netplay::platform {
namespace {

std::atomic<PortMapState> sState{PortMapState::Idle};
std::atomic<bool> sStopRequested{false};
std::thread sWorker;
std::mutex sMutex;
std::string sExternalIp;
std::string sControlUrl;
std::string sServiceType;
uint16_t sTcpPort = 0;
uint16_t sUdpPort = 0;

#ifdef __vita__

struct Url {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
    bool valid = false;
};

Url ParseUrl(const std::string& in) {
    Url url;
    std::string s = in;
    const std::string prefix = "http://";
    if (s.rfind(prefix, 0) != 0) return url;
    s = s.substr(prefix.size());

    const std::size_t slash = s.find('/');
    std::string authority = (slash == std::string::npos) ? s : s.substr(0, slash);
    url.path = (slash == std::string::npos) ? "/" : s.substr(slash);

    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        url.host = authority;
        url.port = 80;
    } else {
        url.host = authority.substr(0, colon);
        url.port = static_cast<uint16_t>(std::atoi(authority.substr(colon + 1).c_str()));
        if (url.port == 0) url.port = 80;
    }
    url.valid = !url.host.empty();
    return url;
}

std::string ResolveRelative(const Url& base, const std::string& ref) {
    if (ref.rfind("http://", 0) == 0) return ref;

    char buf[256];
    if (!ref.empty() && ref[0] == '/') {
        std::snprintf(buf, sizeof(buf), "http://%s:%u%s", base.host.c_str(), base.port, ref.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "http://%s:%u/%s", base.host.c_str(), base.port, ref.c_str());
    }
    return buf;
}

int OpenTcp(const std::string& host, uint16_t port, int timeoutMs) {
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) return -1;

    const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;
    if (::connect(sock, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        ::close(sock);
        return -1;
    }
    return sock;
}

bool SendAll(int sock, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const long n = ::send(sock, data + off, len - off, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Returns the full HTTP response (headers + body). Body is separated by the
// first blank line; callers scan the whole buffer.
std::string HttpRequest(const std::string& host, uint16_t port, const std::string& method,
                        const std::string& path, const std::string& soapAction,
                        const std::string& body) {
    const int sock = OpenTcp(host, port, 4000);
    if (sock < 0) return {};

    char header[1024];
    int hlen = std::snprintf(header, sizeof(header),
                             "%s %s HTTP/1.1\r\n"
                             "Host: %s:%u\r\n"
                             "Connection: close\r\n"
                             "User-Agent: SSB64Vita/1\r\n",
                             method.c_str(), path.c_str(), host.c_str(), port);
    if (!body.empty()) {
        hlen += std::snprintf(header + hlen, sizeof(header) - hlen,
                              "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                              "Content-Length: %u\r\n",
                              static_cast<unsigned>(body.size()));
    }
    if (!soapAction.empty()) {
        hlen += std::snprintf(header + hlen, sizeof(header) - hlen,
                              "SOAPAction: \"%s\"\r\n", soapAction.c_str());
    }
    hlen += std::snprintf(header + hlen, sizeof(header) - hlen, "\r\n");

    if (hlen <= 0 || hlen >= static_cast<int>(sizeof(header)) ||
        !SendAll(sock, header, static_cast<std::size_t>(hlen)) ||
        (!body.empty() && !SendAll(sock, body.data(), body.size()))) {
        ::close(sock);
        return {};
    }

    std::string response;
    char chunk[1024];
    for (;;) {
        const long n = ::recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        response.append(chunk, static_cast<std::size_t>(n));
        if (response.size() > 64 * 1024) break;
    }
    ::close(sock);
    return response;
}

std::string TagValue(const std::string& xml, const std::string& tag) {
    const std::size_t start = xml.find("<" + tag);
    if (start == std::string::npos) return {};
    const std::size_t gt = xml.find('>', start);
    if (gt == std::string::npos) return {};
    const std::size_t close = xml.find("</", gt);
    if (close == std::string::npos) return {};
    return xml.substr(gt + 1, close - gt - 1);
}

bool DiscoverGateway(const std::string& localIp, std::string& outLocation) {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        port_log("[NETPLAY] UPnP ssdp socket failed errno=%d\n", errno);
        return false;
    }

    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 500 * 1000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    in_addr localAddr{};
    if (inet_pton(AF_INET, localIp.c_str(), &localAddr) == 1) {
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr = localAddr;
        bindAddr.sin_port = 0;
        if (::bind(sock, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            port_log("[NETPLAY] UPnP ssdp bind %s failed errno=%d\n", localIp.c_str(), errno);
        }
        ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &localAddr, sizeof(localAddr));
    }
    const unsigned char ttl = 4;
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    sockaddr_in mcast{};
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &mcast.sin_addr);

    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(1900);
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    static const char* kSearchTargets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
        "ssdp:all",
    };

    char msg[256];
    int sent = 0;
    for (int attempt = 0; attempt < 4 && !sStopRequested.load(); ++attempt) {
        for (const char* st : kSearchTargets) {
            const int len = std::snprintf(msg, sizeof(msg),
                                          "M-SEARCH * HTTP/1.1\r\n"
                                          "HOST: 239.255.255.250:1900\r\n"
                                          "MAN: \"ssdp:discover\"\r\n"
                                          "MX: 2\r\n"
                                          "ST: %s\r\n\r\n",
                                          st);
            if (::sendto(sock, msg, static_cast<std::size_t>(len), 0,
                         reinterpret_cast<const sockaddr*>(&mcast), sizeof(mcast)) > 0) {
                ++sent;
            }
            ::sendto(sock, msg, static_cast<std::size_t>(len), 0,
                     reinterpret_cast<const sockaddr*>(&bcast), sizeof(bcast));
        }

        for (int r = 0; r < 12; ++r) {
            char buf[1600];
            const long n = ::recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';

            std::string resp(buf, static_cast<std::size_t>(n));
            const char* keys[] = {"LOCATION:", "Location:", "location:"};
            for (const char* key : keys) {
                const std::size_t pos = resp.find(key);
                if (pos == std::string::npos) continue;
                std::size_t s = pos + std::strlen(key);
                while (s < resp.size() && (resp[s] == ' ' || resp[s] == '\t')) ++s;
                std::size_t e = resp.find_first_of("\r\n", s);
                if (e == std::string::npos) e = resp.size();
                outLocation = resp.substr(s, e - s);
                ::close(sock);
                return !outLocation.empty();
            }
        }
    }
    port_log("[NETPLAY] UPnP ssdp no reply (sent=%d)\n", sent);
    ::close(sock);
    return false;
}

bool FetchServiceInfo(const Url& descUrl) {
    const std::string body = HttpRequest(descUrl.host, descUrl.port, "GET", descUrl.path, "", "");
    if (body.empty()) return false;

    const char* wanTypes[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    for (const char* wt : wanTypes) {
        const std::size_t typePos = body.find(wt);
        if (typePos == std::string::npos) continue;

        // controlURL usually follows serviceType within the same <service> block.
        const std::size_t ctrlPos = body.find("<controlURL>", typePos);
        if (ctrlPos == std::string::npos) continue;
        const std::size_t ctrlStart = ctrlPos + std::strlen("<controlURL>");
        const std::size_t ctrlEnd = body.find("</controlURL>", ctrlStart);
        if (ctrlEnd == std::string::npos) continue;

        std::string controlRef = body.substr(ctrlStart, ctrlEnd - ctrlStart);

        const std::string urlBase = TagValue(body, "URLBase");
        Url baseUrl = urlBase.empty() ? descUrl : ParseUrl(urlBase);
        if (!baseUrl.valid) baseUrl = descUrl;

        std::lock_guard<std::mutex> lock(sMutex);
        sControlUrl = ResolveRelative(baseUrl, controlRef);
        sServiceType = wt;
        return true;
    }
    return false;
}

std::string SoapCall(const std::string& action, const std::string& innerArgs) {
    std::string controlUrl;
    std::string serviceType;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        controlUrl = sControlUrl;
        serviceType = sServiceType;
    }
    Url u = ParseUrl(controlUrl);
    if (!u.valid) return {};

    std::string body =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:" + action + " xmlns:u=\"" + serviceType + "\">" + innerArgs +
        "</u:" + action + ">"
        "</s:Body></s:Envelope>";

    const std::string soapAction = serviceType + "#" + action;
    return HttpRequest(u.host, u.port, "POST", u.path, soapAction, body);
}

bool AddMapping(const std::string& proto, uint16_t port, const std::string& localIp) {
    char args[512];
    std::snprintf(args, sizeof(args),
                  "<NewRemoteHost></NewRemoteHost>"
                  "<NewExternalPort>%u</NewExternalPort>"
                  "<NewProtocol>%s</NewProtocol>"
                  "<NewInternalPort>%u</NewInternalPort>"
                  "<NewInternalClient>%s</NewInternalClient>"
                  "<NewEnabled>1</NewEnabled>"
                  "<NewPortMappingDescription>SSB64VITA</NewPortMappingDescription>"
                  "<NewLeaseDuration>0</NewLeaseDuration>",
                  port, proto.c_str(), port, localIp.c_str());
    std::string resp = SoapCall("AddPortMapping", args);
    if (resp.empty()) {
        port_log("[NETPLAY] UPnP AddPortMapping %s/%u no response\n", proto.c_str(), port);
        return false;
    }
    const bool ok = resp.find(" 200 ") != std::string::npos &&
                    resp.find("AddPortMappingResponse") != std::string::npos;
    if (!ok) {
        const std::string err = TagValue(resp, "errorCode");
        port_log("[NETPLAY] UPnP AddPortMapping %s/%u failed upnp_err=%s\n",
                 proto.c_str(), port, err.empty() ? "?" : err.c_str());

        if (err == "725") {
            // OnlyPermanentLeasesSupported is inverted on some IGDs: retry with
            // a finite lease instead of 0.
            char args2[512];
            std::snprintf(args2, sizeof(args2),
                          "<NewRemoteHost></NewRemoteHost>"
                          "<NewExternalPort>%u</NewExternalPort>"
                          "<NewProtocol>%s</NewProtocol>"
                          "<NewInternalPort>%u</NewInternalPort>"
                          "<NewInternalClient>%s</NewInternalClient>"
                          "<NewEnabled>1</NewEnabled>"
                          "<NewPortMappingDescription>SSB64VITA</NewPortMappingDescription>"
                          "<NewLeaseDuration>3600</NewLeaseDuration>",
                          port, proto.c_str(), port, localIp.c_str());
            resp = SoapCall("AddPortMapping", args2);
            const bool ok2 = resp.find(" 200 ") != std::string::npos &&
                             resp.find("AddPortMappingResponse") != std::string::npos;
            port_log("[NETPLAY] UPnP AddPortMapping %s/%u lease-retry ok=%d\n",
                     proto.c_str(), port, ok2 ? 1 : 0);
            return ok2;
        }
    }
    return ok;
}

void DeleteMapping(const std::string& proto, uint16_t port) {
    char args[256];
    std::snprintf(args, sizeof(args),
                  "<NewRemoteHost></NewRemoteHost>"
                  "<NewExternalPort>%u</NewExternalPort>"
                  "<NewProtocol>%s</NewProtocol>",
                  port, proto.c_str());
    SoapCall("DeletePortMapping", args);
}

void WorkerMain(uint16_t tcpPort, uint16_t udpPort, std::string localIp) {
    sState.store(PortMapState::Working, std::memory_order_release);

    port_log("[NETPLAY] UPnP port-map worker start local=%s tcp=%u udp=%u\n",
             localIp.c_str(), tcpPort, udpPort);

    std::string location;
    if (sStopRequested.load() || !DiscoverGateway(localIp, location)) {
        port_log("[NETPLAY] UPnP gateway not found\n");
        sState.store(PortMapState::Failed, std::memory_order_release);
        return;
    }
    port_log("[NETPLAY] UPnP gateway desc=%s\n", location.c_str());

    Url descUrl = ParseUrl(location);
    if (!descUrl.valid || sStopRequested.load() || !FetchServiceInfo(descUrl)) {
        port_log("[NETPLAY] UPnP WAN service not found\n");
        sState.store(PortMapState::Failed, std::memory_order_release);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        port_log("[NETPLAY] UPnP control=%s type=%s\n", sControlUrl.c_str(), sServiceType.c_str());
    }

    const std::string extResp = SoapCall("GetExternalIPAddress", "");
    const std::string ext = TagValue(extResp, "NewExternalIPAddress");
    if (!ext.empty()) {
        std::lock_guard<std::mutex> lock(sMutex);
        sExternalIp = ext;
        port_log("[NETPLAY] UPnP external ip=%s\n", ext.c_str());
    }

    if (sStopRequested.load()) {
        sState.store(PortMapState::Failed, std::memory_order_release);
        return;
    }

    const bool tcpOk = AddMapping("TCP", tcpPort, localIp);
    const bool udpOk = AddMapping("UDP", udpPort, localIp);

    sState.store((tcpOk && udpOk) ? PortMapState::Mapped : PortMapState::Failed,
                 std::memory_order_release);
    port_log("[NETPLAY] UPnP mapping tcp=%d udp=%d\n", tcpOk ? 1 : 0, udpOk ? 1 : 0);
}

#endif // __vita__

} // namespace

void RequestPortMapping(uint16_t tcpPort, uint16_t udpPort, const std::string& localIp) {
#ifdef __vita__
    if (sState.load(std::memory_order_acquire) == PortMapState::Working) return;
    if (sWorker.joinable()) {
        sStopRequested.store(true);
        sWorker.join();
    }
    sStopRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sExternalIp.clear();
        sControlUrl.clear();
        sServiceType.clear();
    }
    sTcpPort = tcpPort;
    sUdpPort = udpPort;
    sState.store(PortMapState::Working, std::memory_order_release);
    sWorker = std::thread(WorkerMain, tcpPort, udpPort, localIp);
#else
    (void)tcpPort;
    (void)udpPort;
    (void)localIp;
    sState.store(PortMapState::Failed, std::memory_order_release);
#endif
}

PortMapState GetPortMapState() {
    return sState.load(std::memory_order_acquire);
}

std::string GetMappedExternalIp() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sExternalIp;
}

void ReleasePortMapping() {
#ifdef __vita__
    sStopRequested.store(true);
    if (sWorker.joinable()) sWorker.join();

    if (sState.load(std::memory_order_acquire) == PortMapState::Mapped) {
        DeleteMapping("TCP", sTcpPort);
        DeleteMapping("UDP", sUdpPort);
    }
#endif
    sStopRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sExternalIp.clear();
        sControlUrl.clear();
        sServiceType.clear();
    }
    sState.store(PortMapState::Idle, std::memory_order_release);
}

} // namespace ssb64::netplay::platform
