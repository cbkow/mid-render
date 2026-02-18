#include "monitor/peer_manager.h"
#include "core/monitor_log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <chrono>
#include <algorithm>

namespace MR {

static int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void PeerManager::start(const std::filesystem::path& farmPath,
                        const std::string& nodeId,
                        const std::string& localEndpoint,
                        int localPriority,
                        const std::vector<std::string>& localTags)
{
    if (m_running.load())
        return;

    m_farmPath = farmPath;
    m_nodeId = nodeId;
    m_localEndpoint = localEndpoint;
    m_localPriority = localPriority;
    m_localTags = localTags;

    // Write initial endpoint immediately
    writeEndpoint();

    m_running.store(true);
    m_thread = std::thread(&PeerManager::threadFunc, this);
}

void PeerManager::stop()
{
    if (!m_running.load())
        return;

    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();

    removeEndpoint();
}

std::vector<PeerInfo> PeerManager::getPeerSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PeerInfo> result;
    result.reserve(m_peers.size());
    for (const auto& [id, info] : m_peers)
        result.push_back(info);
    return result;
}

std::string PeerManager::leaderId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_leaderId;
}

void PeerManager::setRenderState(const std::string& state,
                                 const std::string& jobId,
                                 const std::string& chunk)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_renderState = state;
    m_activeJob = jobId;
    m_activeChunk = chunk;
}

void PeerManager::setNodeState(const std::string& state)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nodeState = state;
}

void PeerManager::setLocalPriority(int priority)
{
    m_localPriority = priority;
}

void PeerManager::setPeerNodeState(const std::string& nodeId, const std::string& state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_peers.find(nodeId);
    if (it != m_peers.end())
        it->second.node_state = state;
}

void PeerManager::processUdpHeartbeat(const std::string& nodeId, const std::string& ip,
                                       uint16_t port, const std::string& nodeState,
                                       const std::string& renderState, const std::string& jobId,
                                       const std::string& chunk, int priority)
{
    auto now = nowMs();
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_peers.find(nodeId);
    if (it == m_peers.end())
    {
        // New peer discovered via UDP — create minimal entry
        // (last_seen_ms stays 0 until first HTTP poll fills hardware info)
        PeerInfo info;
        info.node_id = nodeId;
        info.endpoint = ip + ":" + std::to_string(port);
        info.node_state = nodeState;
        info.render_state = renderState;
        info.active_job = jobId;
        info.active_chunk = chunk;
        info.priority = priority;
        info.is_alive = true;
        info.failed_polls = 0;
        info.last_seen_ms = 0;
        info.has_udp_contact = true;
        info.last_udp_contact_ms = now;
        m_peers[nodeId] = info;
        MonitorLog::instance().info("peer", "Discovered peer via UDP: " + nodeId +
            " at " + info.endpoint);
    }
    else
    {
        // Update existing peer with fast state info
        // (don't touch last_seen_ms — that tracks HTTP poll success for adaptive polling)
        it->second.node_state = nodeState;
        it->second.render_state = renderState;
        it->second.active_job = jobId;
        it->second.active_chunk = chunk;
        it->second.priority = priority;
        it->second.is_alive = true;
        it->second.failed_polls = 0;
        it->second.has_udp_contact = true;
        it->second.last_udp_contact_ms = now;

        // Update endpoint if changed
        std::string newEndpoint = ip + ":" + std::to_string(port);
        if (it->second.endpoint != newEndpoint)
            it->second.endpoint = newEndpoint;
    }
}

void PeerManager::processUdpGoodbye(const std::string& nodeId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_peers.find(nodeId);
    if (it != m_peers.end())
    {
        it->second.is_alive = false;
        it->second.has_udp_contact = false;
        MonitorLog::instance().info("peer", "Peer goodbye via UDP: " + nodeId);
    }
}

// --- Background thread ---

void PeerManager::threadFunc()
{
    MonitorLog::instance().info("peer", "PeerManager started (endpoint: " + m_localEndpoint + ")");

    while (m_running.load())
    {
        writeEndpoint();
        discoverPeers();
        pollPeers();
        recomputeLeader();

        // Sleep ~3 seconds, checking for stop every 100ms
        for (int i = 0; i < 30 && m_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    MonitorLog::instance().info("peer", "PeerManager stopped");
}

void PeerManager::writeEndpoint()
{
    auto nodeDir = m_farmPath / "nodes" / m_nodeId;
    std::error_code ec;
    std::filesystem::create_directories(nodeDir, ec);

    auto endpointPath = nodeDir / "endpoint.json";

    // Parse ip:port from m_localEndpoint
    std::string ip = m_localEndpoint;
    uint16_t port = 8420;
    auto colonPos = m_localEndpoint.rfind(':');
    if (colonPos != std::string::npos)
    {
        ip = m_localEndpoint.substr(0, colonPos);
        port = static_cast<uint16_t>(std::stoi(m_localEndpoint.substr(colonPos + 1)));
    }

    PeerEndpoint ep;
    ep.node_id = m_nodeId;
    ep.ip = ip;
    ep.port = port;
    ep.timestamp_ms = nowMs();

    try
    {
        nlohmann::json j = ep;
        // Write to temp file then rename for atomicity
        auto tmpPath = endpointPath;
        tmpPath += ".tmp";
        {
            std::ofstream ofs(tmpPath);
            ofs << j.dump(2);
        }
        std::filesystem::rename(tmpPath, endpointPath, ec);
        if (ec)
        {
            // Fallback: direct write (rename can fail across filesystems)
            std::ofstream ofs(endpointPath);
            ofs << j.dump(2);
        }
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("peer", std::string("Failed to write endpoint.json: ") + e.what());
    }
}

void PeerManager::removeEndpoint()
{
    auto endpointPath = m_farmPath / "nodes" / m_nodeId / "endpoint.json";
    std::error_code ec;
    std::filesystem::remove(endpointPath, ec);
}

void PeerManager::discoverPeers()
{
    auto nodesDir = m_farmPath / "nodes";
    std::error_code ec;
    if (!std::filesystem::is_directory(nodesDir, ec))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(nodesDir, ec))
    {
        if (!entry.is_directory())
            continue;

        auto nodeId = entry.path().filename().string();
        if (nodeId == m_nodeId)
            continue;  // skip self

        auto endpointPath = entry.path() / "endpoint.json";
        if (!std::filesystem::exists(endpointPath, ec))
            continue;

        // Already known?
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_peers.count(nodeId))
                continue;
        }

        // Read and parse endpoint.json
        try
        {
            std::ifstream ifs(endpointPath);
            if (!ifs.good())
                continue;

            auto j = nlohmann::json::parse(ifs);
            PeerEndpoint ep = j.get<PeerEndpoint>();

            PeerInfo info;
            info.node_id = ep.node_id;
            info.endpoint = ep.ip + ":" + std::to_string(ep.port);
            info.is_alive = true;
            info.failed_polls = 0;
            info.last_seen_ms = 0;  // not yet polled

            std::lock_guard<std::mutex> lock(m_mutex);
            m_peers[nodeId] = info;
            MonitorLog::instance().info("peer", "Discovered peer: " + nodeId +
                " at " + info.endpoint);
        }
        catch (const std::exception&)
        {
            // Malformed endpoint.json — skip
        }
    }
}

void PeerManager::pollPeers()
{
    auto now = nowMs();

    // Clear UDP contact for peers with >15s of UDP silence
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, info] : m_peers)
        {
            if (info.has_udp_contact && (now - info.last_udp_contact_ms > 15000))
            {
                info.has_udp_contact = false;
                MonitorLog::instance().info("peer", "UDP contact lost for: " + id);
            }

            // UDP silence >10s on a UDP-contacted peer = dead
            if (info.is_alive && info.last_udp_contact_ms > 0 &&
                !info.has_udp_contact && (now - info.last_udp_contact_ms > 10000))
            {
                // Only mark dead if also failing HTTP polls (belt + suspenders)
                // The 15s UDP clear above + 3 failed HTTP polls covers this
            }
        }
    }

    // Take a snapshot of peers to poll (avoid holding lock during HTTP calls)
    // Adaptive: peers with UDP contact get polled every ~9s (skip 2 out of 3 cycles)
    std::vector<std::pair<std::string, std::string>> toCheck;  // nodeId, endpoint
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, info] : m_peers)
        {
            if (info.has_udp_contact && !info.hostname.empty())
            {
                // Skip HTTP poll if last successful HTTP poll was within 9s
                // (always poll at least once to get full hardware info)
                if (info.last_seen_ms > 0 && (now - info.last_seen_ms < 9000))
                    continue;
            }
            toCheck.emplace_back(id, info.endpoint);
        }
    }

    for (const auto& [nodeId, endpoint] : toCheck)
    {
        if (!m_running.load())
            return;

        // Parse ip:port
        std::string host = endpoint;
        int port = 8420;
        auto colonPos = endpoint.rfind(':');
        if (colonPos != std::string::npos)
        {
            host = endpoint.substr(0, colonPos);
            port = std::stoi(endpoint.substr(colonPos + 1));
        }

        // Fresh client per call (httplib::Client is not safe for concurrent reuse)
        httplib::Client cli(host, port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(3);

        auto res = cli.Get("/api/status");

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_peers.find(nodeId);
        if (it == m_peers.end())
            continue;

        if (res && res->status == 200)
        {
            try
            {
                auto j = nlohmann::json::parse(res->body);
                PeerInfo updated = j.get<PeerInfo>();

                // Preserve runtime fields
                updated.is_local = false;
                updated.is_alive = true;
                updated.is_leader = it->second.is_leader;
                updated.failed_polls = 0;
                updated.last_seen_ms = nowMs();
                updated.has_udp_contact = it->second.has_udp_contact;
                updated.last_udp_contact_ms = it->second.last_udp_contact_ms;

                it->second = updated;
            }
            catch (const std::exception&)
            {
                // Bad JSON — count as failure
                it->second.failed_polls++;
            }
        }
        else
        {
            it->second.failed_polls++;
        }

        // 3 consecutive failures = dead
        if (it->second.failed_polls >= 3 && it->second.is_alive)
        {
            it->second.is_alive = false;
            MonitorLog::instance().warn("peer", "Peer dead: " + nodeId +
                " (" + std::to_string(it->second.failed_polls) + " failed polls)");
        }
    }

    // Remove peers whose endpoint.json is deleted AND are dead
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_peers.begin(); it != m_peers.end(); )
        {
            if (!it->second.is_alive)
            {
                auto endpointPath = m_farmPath / "nodes" / it->first / "endpoint.json";
                std::error_code ec;
                if (!std::filesystem::exists(endpointPath, ec))
                {
                    MonitorLog::instance().info("peer", "Removed stale peer: " + it->first);
                    it = m_peers.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
}

void PeerManager::recomputeLeader()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Collect alive candidates: self + alive peers (including stopped nodes as fallback).
    // Stopped nodes can still lead (coordinate dispatch) even though they don't render.
    // Election rank: "leader" tag > NOT "noleader" tag > alphabetical node_id.
    // Deterministic sort — same inputs always produce the same winner.
    struct Candidate { std::string id; bool hasLeaderTag; bool hasNoLeaderTag; };
    std::vector<Candidate> candidates;

    // Self is always a candidate
    {
        std::lock_guard<std::mutex> slock(m_stateMutex);
        bool selfLeader = std::find(m_localTags.begin(), m_localTags.end(),
                                    "leader") != m_localTags.end();
        bool selfNoLeader = std::find(m_localTags.begin(), m_localTags.end(),
                                      "noleader") != m_localTags.end();
        candidates.push_back({m_nodeId, selfLeader, selfNoLeader});
    }

    for (const auto& [id, info] : m_peers)
    {
        if (info.is_alive)
        {
            bool peerLeader = std::find(info.tags.begin(), info.tags.end(),
                                        "leader") != info.tags.end();
            bool peerNoLeader = std::find(info.tags.begin(), info.tags.end(),
                                          "noleader") != info.tags.end();
            candidates.push_back({id, peerLeader, peerNoLeader});
        }
    }

    if (candidates.empty())
    {
        m_leaderId.clear();
        m_isLeader.store(false);
        return;
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
    {
        // "leader" tagged nodes first
        if (a.hasLeaderTag != b.hasLeaderTag)
            return a.hasLeaderTag > b.hasLeaderTag;
        // "noleader" tagged nodes last
        if (a.hasNoLeaderTag != b.hasNoLeaderTag)
            return a.hasNoLeaderTag < b.hasNoLeaderTag;
        // Alphabetical tiebreak
        return a.id < b.id;
    });

    std::string newLeader = candidates[0].id;
    bool nowLeader = (newLeader == m_nodeId);

    if (newLeader != m_leaderId)
    {
        m_leaderId = newLeader;
        MonitorLog::instance().info("peer", "Leader elected: " + newLeader +
            (nowLeader ? " (this node)" : ""));
    }

    m_isLeader.store(nowLeader);

    // Update leader flags on peers
    for (auto& [id, info] : m_peers)
        info.is_leader = (id == newLeader);
}

} // namespace MR
