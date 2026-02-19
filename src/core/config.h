#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace MR {

struct Config
{
    // Sync root path (shared filesystem mount point)
    std::string sync_root;

    // Leader election priority (lower = higher priority)
    int priority = 100;

    // HTTP mesh port
    uint16_t http_port = 8420;

    // IP override (empty = auto-detect)
    std::string ip_override;

    // Node tags (for job targeting)
    std::vector<std::string> tags;

    // Agent settings
    bool auto_start_agent = true;

    // UDP multicast
    bool udp_enabled = true;
    uint16_t udp_port = 4243;

    // UI preferences
    bool show_notifications = true;
    float font_scale = 1.0f;

    // Rendering
    bool staging_enabled = false;

    // Persisted node state
    bool node_stopped = false;
};

// JSON serialization
inline void to_json(nlohmann::json& j, const Config& c)
{
    j = nlohmann::json{
        {"sync_root", c.sync_root},
        {"priority", c.priority},
        {"http_port", c.http_port},
        {"ip_override", c.ip_override},
        {"tags", c.tags},
        {"auto_start_agent", c.auto_start_agent},
        {"udp_enabled", c.udp_enabled},
        {"udp_port", c.udp_port},
        {"show_notifications", c.show_notifications},
        {"font_scale", c.font_scale},
        {"staging_enabled", c.staging_enabled},
        {"node_stopped", c.node_stopped},
    };
}

inline void from_json(const nlohmann::json& j, Config& c)
{
    if (j.contains("sync_root"))         j.at("sync_root").get_to(c.sync_root);
    if (j.contains("priority"))          j.at("priority").get_to(c.priority);
    if (j.contains("http_port"))         c.http_port = j.at("http_port").get<uint16_t>();
    if (j.contains("ip_override"))       j.at("ip_override").get_to(c.ip_override);
    if (j.contains("tags"))              j.at("tags").get_to(c.tags);
    if (j.contains("auto_start_agent"))  j.at("auto_start_agent").get_to(c.auto_start_agent);
    if (j.contains("udp_enabled"))       j.at("udp_enabled").get_to(c.udp_enabled);
    if (j.contains("udp_port"))          c.udp_port = j.at("udp_port").get<uint16_t>();
    if (j.contains("show_notifications")) j.at("show_notifications").get_to(c.show_notifications);
    if (j.contains("font_scale"))         j.at("font_scale").get_to(c.font_scale);
    if (j.contains("staging_enabled"))    j.at("staging_enabled").get_to(c.staging_enabled);
    if (j.contains("node_stopped"))       j.at("node_stopped").get_to(c.node_stopped);
}

// --- Constants ---
constexpr uint32_t PROTOCOL_VERSION = 2;

#ifndef APP_VERSION
#define APP_VERSION "0.2.5"
#endif

} // namespace MR
