#pragma once

#include "core/job_types.h"
#include "monitor/node_failure_tracker.h"

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <chrono>
#include <cstdint>

namespace MR {

class MonitorApp;
class DatabaseManager;

struct CompletionReport
{
    std::string node_id, job_id;
    int frame_start = 0, frame_end = 0;
    int64_t elapsed_ms = 0;
    int exit_code = 0;
};

struct FailureReport
{
    std::string node_id, job_id;
    int frame_start = 0, frame_end = 0;
    std::string error;
};

struct FrameReport
{
    std::string node_id, job_id;
    int frame = 0;
};

struct SubmitRequest
{
    JobManifest manifest;
    int priority = 50;
};

class DispatchManager
{
public:
    void init(MonitorApp* app, DatabaseManager* db);

    // Main thread, self-throttled to ~2s
    void update();

    // Thread-safe queues (HTTP handlers -> main thread)
    void queueCompletion(CompletionReport report);
    void queueFailure(FailureReport report);
    void queueSubmission(SubmitRequest request);
    void queueFrameCompletion(FrameReport report);

    // Direct submission (main thread, for local leader submit)
    std::string submitJob(const JobManifest& manifest, int priority);

    // Retry only failed chunks (preserves completed work, keeps blacklist)
    bool retryFailedChunks(const std::string& jobId);

    // Create a new job from an existing job's manifest (clean slate)
    std::string resubmitJob(const std::string& sourceJobId);

    // Machine-level failure tracking
    NodeFailureTracker& failureTracker() { return m_failureTracker; }

private:
    void processSubmissions();
    void processReports();
    void detectDeadWorkers();
    void checkJobCompletions();
    void assignWork();
    void doSnapshot();

    MonitorApp* m_app = nullptr;
    DatabaseManager* m_db = nullptr;

    std::chrono::steady_clock::time_point m_lastDispatch;
    std::chrono::steady_clock::time_point m_lastSnapshot;

    // Thread-safe queues
    std::mutex m_queueMutex;
    std::queue<CompletionReport> m_completionQueue;
    std::queue<FailureReport> m_failureQueue;
    std::queue<SubmitRequest> m_submitQueue;
    std::queue<FrameReport> m_frameQueue;

    NodeFailureTracker m_failureTracker;

    static constexpr int DISPATCH_INTERVAL_MS = 2000;
    static constexpr int SNAPSHOT_INTERVAL_MS = 30000;
};

} // namespace MR
