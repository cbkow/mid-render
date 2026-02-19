#pragma once

#include "core/job_types.h"

#include <filesystem>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <set>
#include <optional>
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace MR {

class AgentSupervisor;

class RenderCoordinator
{
public:
    using CompletionCallback = std::function<void(const std::string& jobId,
                                                   const ChunkRange& chunk,
                                                   const std::string& state)>;
    using FrameCompletionCallback = std::function<void(const std::string& jobId, int frame)>;

    RenderCoordinator() = default;

    void init(const std::filesystem::path& farmPath, const std::string& nodeId,
              const std::string& nodeOS, CompletionCallback completionFn,
              FrameCompletionCallback frameCompletionFn,
              AgentSupervisor* supervisor);

    // Called by DispatchManager — thread-safe
    void queueDispatch(const JobManifest& manifest, const ChunkRange& chunk);

    // Called from MonitorApp::update() (main thread)
    void update(AgentSupervisor& supervisor);

    // Called from AgentSupervisor message handler (main thread)
    void handleAgentMessage(const std::string& type, const nlohmann::json& j);

    // Abort (kill-only — no drain concept)
    void abortCurrentRender(const std::string& reason);
    void purgeJob(const std::string& jobId);  // Remove queued (not yet active) chunks for a job
    void setStopped(bool stopped);
    bool isStopped() const { return m_stopped; }
    void setStagingEnabled(bool enabled);

    // UI queries
    bool isRendering() const { return m_activeRender.has_value(); }
    std::string currentJobId() const;
    ChunkRange currentChunk() const;
    std::string currentChunkLabel() const;  // "f42" or "f42-50"
    float currentProgress() const;

private:
    // Task JSON building + dispatch
    nlohmann::json buildTaskJson(const JobManifest& manifest, const ChunkRange& chunk);
    void dispatchChunk(AgentSupervisor& supervisor);
    std::string substituteTokens(const std::string& input, const ChunkRange& chunk) const;

    // Stdout log files (kept on shared FS for cross-node troubleshooting)
    void flushStdout();
    void appendStdout(const std::vector<std::string>& lines);

    // Completion / failure
    void onChunkCompleted(const nlohmann::json& j);
    void onChunkFailed(const nlohmann::json& j);
    void failChunk(const std::string& error);

    // Staging helpers
    bool copyStagingFiles(const std::string& stagingDir, const std::string& outputDir);

    // Dispatch queue (DispatchManager → main thread)
    struct PendingDispatch
    {
        JobManifest manifest;
        ChunkRange chunk;
    };
    std::queue<PendingDispatch> m_dispatchQueue;
    std::mutex m_queueMutex;

    // Active render state (main thread only)
    struct ActiveRender
    {
        JobManifest manifest;
        ChunkRange chunk;
        bool ackReceived = false;
        float progressPct = 0.0f;
        std::chrono::steady_clock::time_point startTime;
        std::vector<std::string> stdoutBuffer;
        std::string stdoutLogName;  // "{rangeStr}_{timestamp_ms}.log" — set once at dispatch
        std::set<int> completedFrames;
        std::string stagingDir;        // local staging dir for this chunk (empty if staging disabled)
        std::string originalOutputDir; // original manifest.output_dir to copy back to
    };
    std::optional<ActiveRender> m_activeRender;

    // Config
    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    std::string m_nodeOS;
    CompletionCallback m_completionFn;
    FrameCompletionCallback m_frameCompletionFn;
    AgentSupervisor* m_supervisor = nullptr;
    bool m_stopped = false;
    bool m_stagingEnabled = false;
};

} // namespace MR
