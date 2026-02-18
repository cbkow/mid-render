#pragma once

#include "core/config.h"
#include "core/node_identity.h"
#include "core/peer_info.h"
#include "core/http_server.h"
#include "core/udp_notify.h"
#include "monitor/agent_supervisor.h"
#include "monitor/render_coordinator.h"
#include "monitor/template_manager.h"
#include "monitor/peer_manager.h"
#include "monitor/database_manager.h"
#include "monitor/dispatch_manager.h"
#include "monitor/submission_watcher.h"
#include "monitor/ui/dashboard.h"

#include "core/system_tray.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

namespace MR {

enum class NodeState { Active, Stopped };

class MonitorApp
{
public:
    bool init();
    void update();
    void renderUI();
    void shutdown();

    // Config accessors
    Config& config() { return m_config; }
    const Config& config() const { return m_config; }
    const NodeIdentity& identity() const { return m_identity; }
    AgentSupervisor& agentSupervisor() { return m_agentSupervisor; }
    RenderCoordinator& renderCoordinator() { return m_renderCoordinator; }
    PeerManager& peerManager() { return m_peerManager; }
    DatabaseManager& databaseManager() { return m_databaseManager; }
    DispatchManager& dispatchManager() { return m_dispatchManager; }

    // Build this node's PeerInfo (called by HttpServer for GET /api/status)
    PeerInfo buildLocalPeerInfo() const;

    // Cached snapshots for UI
    const std::vector<JobInfo>& cachedJobs() const { return m_cachedJobs; }
    const std::vector<JobTemplate>& cachedTemplates() const { return m_cachedTemplates; }

    // Thread-safe cached JSON for HTTP handlers
    std::string getCachedJobsJson() const;
    std::string getCachedJobDetailJson(const std::string& jobId) const;

    // Chunk data access (leader: DB query, worker: HTTP GET from leader)
    std::vector<ChunkRow> getChunksForJob(const std::string& jobId);

    // Job controls
    void pauseJob(const std::string& jobId);
    void resumeJob(const std::string& jobId);
    void cancelJob(const std::string& jobId);
    void requeueJob(const std::string& jobId);
    void deleteJob(const std::string& jobId);
    void archiveJob(const std::string& jobId);

    // Node state controls
    void setNodeState(NodeState state);
    NodeState nodeState() const { return m_nodeState; }

    // Leader election (delegated to PeerManager)
    bool isLeader() const { return m_peerManager.isLeader(); }
    std::string getLeaderEndpoint() const;

    // Tray state
    TrayIconState trayState() const;
    std::string trayTooltip() const;
    std::string trayStatusText() const;

    // Exit flow
    void requestExit();
    bool isExitPending() const { return m_exitRequested && !m_shouldExit; }
    bool shouldExit() const { return m_shouldExit; }
    void beginForceExit();
    void cancelExit();

    void saveConfig();

    // Farm lifecycle
    bool startFarm();
    void stopFarm();
    bool isFarmRunning() const { return m_farmRunning; }
    const std::filesystem::path& farmPath() const { return m_farmPath; }
    bool hasFarmError() const { return !m_farmError.empty(); }
    const std::string& farmError() const { return m_farmError; }

    // Job selection state
    void selectJob(const std::string& id);
    void requestSubmissionMode();
    const std::string& selectedJobId() const { return m_selectedJobId; }
    bool shouldEnterSubmission();

private:
    void loadConfig();
    void onBecomeLeader();
    void onLoseLeadership();
    void refreshCachedJobs();
    void reportCompletion(const std::string& jobId, const ChunkRange& chunk,
                          const std::string& state);
    void reportFrameCompletion(const std::string& jobId, int frame);
    void handleUdpMessages();
    void sendUdpHeartbeat();

    std::filesystem::path m_appDataDir;
    std::filesystem::path m_configPath;

    NodeIdentity m_identity;
    Config m_config;
    AgentSupervisor m_agentSupervisor;
    RenderCoordinator m_renderCoordinator;
    TemplateManager m_templateManager;
    HttpServer m_httpServer;
    PeerManager m_peerManager;
    DatabaseManager m_databaseManager;
    DispatchManager m_dispatchManager;
    SubmissionWatcher m_submissionWatcher;
    UdpNotify m_udpNotify;
    Dashboard m_dashboard;

    // Cached snapshots
    std::vector<JobInfo> m_cachedJobs;
    std::vector<JobTemplate> m_cachedTemplates;

    // Thread-safe cached JSON for HTTP handlers
    mutable std::mutex m_cachedJsonMutex;
    std::string m_cachedJobsJson = "[]";

    // Farm state
    std::filesystem::path m_farmPath;
    std::string m_farmError;
    bool m_farmRunning = false;
    NodeState m_nodeState = NodeState::Active;

    // Leader tracking
    bool m_wasLeader = false;
    std::atomic<bool> m_leaderDbReady{false};
    std::thread m_leaderThread;

    // Leader contact cooldown — after any failed HTTP to leader, skip for 5s
    std::chrono::steady_clock::time_point m_leaderContactCooldown;

    // Pending completion reports (worker -> leader, buffered on failure)
    struct PendingReport
    {
        std::string jobId;
        int frameStart = 0, frameEnd = 0;
        std::string state;
        int64_t elapsedMs = 0;
        int exitCode = 0;
        std::string error;
    };
    std::vector<PendingReport> m_pendingReports;

    // Pending frame completion reports (worker -> leader)
    struct PendingFrameReport
    {
        std::string jobId;
        int frame = 0;
    };
    std::vector<PendingFrameReport> m_pendingFrameReports;
    std::chrono::steady_clock::time_point m_lastFrameReportFlush;

    // UDP heartbeat timing
    std::chrono::steady_clock::time_point m_lastUdpHeartbeat;

    // Job cache refresh timing
    std::chrono::steady_clock::time_point m_lastJobCacheRefresh;

    // Job selection
    std::string m_selectedJobId;
    bool m_requestSubmission = false;

    // Exit state
    bool m_exitRequested = false;
    bool m_shouldExit = false;
};

} // namespace MR
