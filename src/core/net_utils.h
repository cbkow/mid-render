#pragma once

#include <string>
#include <utility>
#include <cstdint>

namespace MR {

// Returns the first non-loopback IPv4 address, or "127.0.0.1" if none found.
// Windows: GetAdaptersAddresses (Iphlpapi.lib)
// Other:   getifaddrs()
std::string getLocalIpAddress();

// Parse "host:port" → {host, port}. Returns {"", 0} on failure.
std::pair<std::string, int> parseEndpoint(const std::string& endpoint);

} // namespace MR
