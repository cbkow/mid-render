#pragma once

#include "core/peer_info.h"

#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

namespace MR {

class PeerManager
{
public:
    void start(const std::filesystem::path& farmPath,
               const std::string& nodeId,
               const std::string& localEndpoint,   // "ip:port"
               int localPriority,
               const std::vector<std::string>& localTags = {});
    void stop();

    // Thread-safe snapshot for UI and /api/peers
    std::vector<PeerInfo> getPeerSnapshot() const;

    // Thread-safe leader query
    bool isLeader() const { return m_isLeader.load(); }
    std::string leaderId() const;

    // Thread-safe setters (called from main thread when state changes)
    void setRenderState(const std::string& state,
                        const std::string& jobId = {},
                        const std::string& chunk = {});
    void setNodeState(const std::string& state);  // "active" | "stopped"
    void setLocalPriority(int priority);

    // Optimistic update: set a remote peer's node_state locally (instant UI feedback)
    void setPeerNodeState(const std::string& nodeId, const std::string& state);

    // UDP multicast fast path (called from main thread via MonitorApp)
    void processUdpHeartbeat(const std::string& nodeId, const std::string& ip,
                             uint16_t port, const std::string& nodeState,
                             const std::string& renderState, const std::string& jobId,
                             const std::string& chunk, int priority);
    void processUdpGoodbye(const std::string& nodeId);

private:
    void threadFunc();

    // Write own endpoint.json to {farmPath}/nodes/{nodeId}/
    void writeEndpoint();

    // Remove own endpoint.json on shutdown
    void removeEndpoint();

    // Scan {farmPath}/nodes/*/endpoint.json for new peers
    void discoverPeers();

    // HTTP GET /api/status to each known peer
    void pollPeers();

    // Recompute leader from alive peers + self
    void recomputeLeader();

    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    std::string m_localEndpoint;
    int m_localPriority = 100;
    std::vector<std::string> m_localTags;

    mutable std::mutex m_mutex;
    std::map<std::string, PeerInfo> m_peers;  // nodeId → PeerInfo (excludes self)
    std::atomic<bool> m_isLeader{false};
    std::string m_leaderId;  // guarded by m_mutex

    // Local render state (set from main thread, read from HTTP handler thread)
    mutable std::mutex m_stateMutex;
    std::string m_renderState = "idle";
    std::string m_activeJob;
    std::string m_activeChunk;
    std::string m_nodeState = "active";

    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace MR
