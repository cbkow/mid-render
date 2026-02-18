#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace MR {

// Written to {farmPath}/nodes/{nodeId}/endpoint.json for filesystem-based discovery
struct PeerEndpoint
{
    std::string node_id;
    std::string ip;
    uint16_t port = 8420;
    int64_t timestamp_ms = 0;
};

inline void to_json(nlohmann::json& j, const PeerEndpoint& e)
{
    j = nlohmann::json{
        {"node_id", e.node_id},
        {"ip", e.ip},
        {"port", e.port},
        {"timestamp_ms", e.timestamp_ms},
    };
}

inline void from_json(const nlohmann::json& j, PeerEndpoint& e)
{
    if (j.contains("node_id"))      j.at("node_id").get_to(e.node_id);
    if (j.contains("ip"))           j.at("ip").get_to(e.ip);
    if (j.contains("port"))         e.port = j.at("port").get<uint16_t>();
    if (j.contains("timestamp_ms")) j.at("timestamp_ms").get_to(e.timestamp_ms);
}

// Full peer status — returned by GET /api/status and used for UI display
struct PeerInfo
{
    // Identity
    std::string node_id;
    std::string hostname;
    std::string os;
    std::string app_version;

    // Hardware
    std::string gpu_name;
    int cpu_cores = 0;
    uint64_t ram_mb = 0;

    // State
    std::string node_state = "active";   // active | stopped
    std::string render_state = "idle";   // idle | rendering
    std::string active_job;
    std::string active_chunk;
    int priority = 100;
    std::vector<std::string> tags;

    // Network
    std::string endpoint;   // "ip:port"

    // Runtime (not serialized over HTTP, computed locally by PeerManager)
    bool is_local = false;
    bool is_alive = true;
    bool is_leader = false;
    int failed_polls = 0;
    int64_t last_seen_ms = 0;

    // UDP multicast (runtime only, not serialized)
    bool has_udp_contact = false;
    int64_t last_udp_contact_ms = 0;
};

// Serialize only the fields that go over HTTP (skip runtime fields)
inline void to_json(nlohmann::json& j, const PeerInfo& p)
{
    j = nlohmann::json{
        {"node_id",      p.node_id},
        {"hostname",     p.hostname},
        {"os",           p.os},
        {"app_version",  p.app_version},
        {"gpu_name",     p.gpu_name},
        {"cpu_cores",    p.cpu_cores},
        {"ram_mb",       p.ram_mb},
        {"node_state",   p.node_state},
        {"render_state", p.render_state},
        {"active_job",   p.active_job},
        {"active_chunk", p.active_chunk},
        {"priority",     p.priority},
        {"tags",         p.tags},
        {"endpoint",     p.endpoint},
    };
}

inline void from_json(const nlohmann::json& j, PeerInfo& p)
{
    if (j.contains("node_id"))      j.at("node_id").get_to(p.node_id);
    if (j.contains("hostname"))     j.at("hostname").get_to(p.hostname);
    if (j.contains("os"))           j.at("os").get_to(p.os);
    if (j.contains("app_version"))  j.at("app_version").get_to(p.app_version);
    if (j.contains("gpu_name"))     j.at("gpu_name").get_to(p.gpu_name);
    if (j.contains("cpu_cores"))    j.at("cpu_cores").get_to(p.cpu_cores);
    if (j.contains("ram_mb"))       j.at("ram_mb").get_to(p.ram_mb);
    if (j.contains("node_state"))   j.at("node_state").get_to(p.node_state);
    if (j.contains("render_state")) j.at("render_state").get_to(p.render_state);
    if (j.contains("active_job"))   j.at("active_job").get_to(p.active_job);
    if (j.contains("active_chunk")) j.at("active_chunk").get_to(p.active_chunk);
    if (j.contains("priority"))     j.at("priority").get_to(p.priority);
    if (j.contains("tags"))         j.at("tags").get_to(p.tags);
    if (j.contains("endpoint"))     j.at("endpoint").get_to(p.endpoint);
}

} // namespace MR
