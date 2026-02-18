#include "core/net_utils.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#else
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <cstring>

namespace MR {

std::string getLocalIpAddress()
{
#ifdef _WIN32
    ULONG bufSize = 15000;
    std::vector<BYTE> buf(bufSize);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

    ULONG ret = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &bufSize);

    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        buf.resize(bufSize);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        ret = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufSize);
    }

    if (ret != NO_ERROR)
        return "127.0.0.1";

    for (auto* adapter = addrs; adapter; adapter = adapter->Next)
    {
        // Skip loopback, tunnel, and down adapters
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next)
        {
            if (ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;

            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ipStr[INET_ADDRSTRLEN] = {};
            InetNtopA(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

            // Skip loopback
            if (std::strcmp(ipStr, "127.0.0.1") == 0)
                continue;

            return ipStr;
        }
    }

    return "127.0.0.1";

#else
    struct ifaddrs* ifList = nullptr;
    if (getifaddrs(&ifList) != 0)
        return "127.0.0.1";

    std::string result = "127.0.0.1";
    for (auto* ifa = ifList; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;

        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

        if (std::strcmp(ipStr, "127.0.0.1") == 0)
            continue;

        result = ipStr;
        break;
    }

    freeifaddrs(ifList);
    return result;
#endif
}

std::pair<std::string, int> parseEndpoint(const std::string& endpoint)
{
    auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == endpoint.size() - 1)
        return {"", 0};

    std::string host = endpoint.substr(0, colon);
    int port = 0;
    try { port = std::stoi(endpoint.substr(colon + 1)); }
    catch (...) { return {"", 0}; }

    if (port <= 0 || port > 65535)
        return {"", 0};

    return {host, port};
}

} // namespace MR
