#include "monitor/dispatch_manager.h"
#include "monitor/database_manager.h"
#include "monitor/monitor_app.h"
#include "core/monitor_log.h"
#include "core/net_utils.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <unordered_map>

namespace MR {

void DispatchManager::init(MonitorApp* app, DatabaseManager* db)
{
    m_app = app;
    m_db = db;
    m_lastDispatch = std::chrono::steady_clock::now();
    m_lastSnapshot = std::chrono::steady_clock::now();
}

void DispatchManager::update()
{
    if (!m_app || !m_db || !m_db->isOpen())
        return;

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastDispatch).count();
    if (elapsedMs < DISPATCH_INTERVAL_MS)
        return;
    m_lastDispatch = now;

    // 1. Drain submit queue
    processSubmissions();

    // 2. Drain completion + failure queues
    processReports();

    // 3. Detect dead workers and reassign their chunks
    detectDeadWorkers();

    // 4. Check if any active jobs are now complete
    checkJobCompletions();

    // 5. Assign work to idle workers
    assignWork();

    // 6. Periodic snapshot
    auto snapshotElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastSnapshot).count();
    if (snapshotElapsed >= SNAPSHOT_INTERVAL_MS)
    {
        doSnapshot();
        m_lastSnapshot = now;
    }
}

// --- Thread-safe queues ---

void DispatchManager::queueCompletion(CompletionReport report)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_completionQueue.push(std::move(report));
}

void DispatchManager::queueFailure(FailureReport report)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_failureQueue.push(std::move(report));
}

void DispatchManager::queueSubmission(SubmitRequest request)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_submitQueue.push(std::move(request));
}

void DispatchManager::queueFrameCompletion(FrameReport report)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_frameQueue.push(std::move(report));
}

// --- Direct submission ---

std::string DispatchManager::submitJob(const JobManifest& manifest, int priority)
{
    if (!m_db || !m_db->isOpen())
        return {};

    // Insert job row
    JobRow row;
    row.job_id = manifest.job_id;
    row.manifest_json = nlohmann::json(manifest).dump();
    row.current_state = "active";
    row.priority = priority;
    row.submitted_at_ms = manifest.submitted_at_ms;

    if (!m_db->insertJob(row))
        return {};

    // Compute and insert chunks
    auto chunks = computeChunks(manifest.frame_start, manifest.frame_end, manifest.chunk_size);
    if (!m_db->insertChunks(manifest.job_id, chunks))
    {
        m_db->deleteJob(manifest.job_id);
        return {};
    }

    MonitorLog::instance().info("dispatch",
        "Job submitted: " + manifest.job_id + " (" + std::to_string(chunks.size()) + " chunks)");

    return manifest.job_id;
}

// --- Internal ---

void DispatchManager::processSubmissions()
{
    std::queue<SubmitRequest> submissions;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::swap(submissions, m_submitQueue);
    }

    while (!submissions.empty())
    {
        auto& req = submissions.front();
        submitJob(req.manifest, req.priority);
        submissions.pop();
    }
}

void DispatchManager::processReports()
{
    // Drain completions
    std::queue<CompletionReport> completions;
    std::queue<FailureReport> failures;
    std::queue<FrameReport> frameReports;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::swap(completions, m_completionQueue);
        std::swap(failures, m_failureQueue);
        std::swap(frameReports, m_frameQueue);
    }

    while (!completions.empty())
    {
        auto& r = completions.front();
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_db->completeChunk(r.job_id, r.frame_start, r.frame_end, nowMs);
        MonitorLog::instance().info("dispatch",
            "Chunk completed: " + r.job_id + " f" + std::to_string(r.frame_start) +
            "-" + std::to_string(r.frame_end) + " by " + r.node_id);
        completions.pop();
    }

    while (!failures.empty())
    {
        auto& r = failures.front();
        // Look up max_retries from the job's manifest
        auto jobOpt = m_db->getJob(r.job_id);
        int maxRetries = 3;
        if (jobOpt.has_value())
        {
            try
            {
                auto manifest = nlohmann::json::parse(jobOpt->manifest_json).get<JobManifest>();
                maxRetries = manifest.max_retries;
            }
            catch (...) {}
        }

        m_db->failChunk(r.job_id, r.frame_start, r.frame_end, maxRetries, r.node_id);

        // Record in machine-level failure tracker
        if (!r.node_id.empty())
        {
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            bool wasSuspended = m_failureTracker.isSuspended(r.node_id);
            m_failureTracker.recordFailure(r.node_id, nowMs);
            if (!wasSuspended && m_failureTracker.isSuspended(r.node_id))
            {
                MonitorLog::instance().warn("dispatch",
                    "Node " + r.node_id + " suspended — too many failures in 5 minutes");
            }
        }

        MonitorLog::instance().warn("dispatch",
            "Chunk failed: " + r.job_id + " f" + std::to_string(r.frame_start) +
            "-" + std::to_string(r.frame_end) + " by " + r.node_id + ": " + r.error);
        failures.pop();
    }

    // Drain frame completions — batch by job_id for efficiency
    if (!frameReports.empty())
    {
        std::unordered_map<std::string, std::vector<int>> byJob;
        while (!frameReports.empty())
        {
            auto& fr = frameReports.front();
            byJob[fr.job_id].push_back(fr.frame);
            frameReports.pop();
        }
        for (auto& [jobId, frames] : byJob)
        {
            m_db->addCompletedFramesBatch(jobId, frames);
        }
    }
}

void DispatchManager::detectDeadWorkers()
{
    auto peers = m_app->peerManager().getPeerSnapshot();
    for (const auto& p : peers)
    {
        if (!p.is_alive && !p.is_local)
        {
            m_db->reassignDeadWorkerChunks(p.node_id);
        }
    }
}

void DispatchManager::checkJobCompletions()
{
    auto jobs = m_db->getAllJobs();
    for (const auto& js : jobs)
    {
        if (js.job.current_state == "active" && m_db->isJobComplete(js.job.job_id))
        {
            m_db->updateJobState(js.job.job_id, "completed");
            MonitorLog::instance().info("dispatch", "Job completed: " + js.job.job_id);
        }
    }
}

void DispatchManager::assignWork()
{
    // Build set of idle, alive, active workers (including self)
    auto peers = m_app->peerManager().getPeerSnapshot();

    // Include local node info
    auto localInfo = m_app->buildLocalPeerInfo();
    peers.push_back(localInfo);

    for (const auto& peer : peers)
    {
        // Skip non-alive, stopped, or already rendering nodes
        if (!peer.is_alive)
            continue;
        if (peer.node_state == "stopped")
            continue;
        if (peer.render_state == "rendering")
            continue;

        // Skip suspended nodes (machine-level failure tracking)
        if (m_failureTracker.isSuspended(peer.node_id))
            continue;

        // Find next pending chunk this peer is eligible for (respects tags + blacklist)
        auto chunkOpt = m_db->findNextPendingChunkForNode(peer.tags, peer.node_id);
        if (!chunkOpt.has_value())
            continue; // no compatible work for this peer (other peers may still match)

        auto& [chunk, manifestJson] = chunkOpt.value();

        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Assign in DB
        if (!m_db->assignChunk(chunk.id, peer.node_id, nowMs))
            continue;

        // Dispatch to worker
        if (peer.is_local)
        {
            // Self-dispatch via RenderCoordinator
            try
            {
                auto manifest = nlohmann::json::parse(manifestJson).get<JobManifest>();
                ChunkRange cr{chunk.frame_start, chunk.frame_end};
                m_app->renderCoordinator().queueDispatch(manifest, cr);
                MonitorLog::instance().info("dispatch",
                    "Self-assigned: " + chunk.job_id + " f" + std::to_string(chunk.frame_start) +
                    "-" + std::to_string(chunk.frame_end));
            }
            catch (const std::exception& e)
            {
                MonitorLog::instance().error("dispatch",
                    std::string("Self-dispatch parse error: ") + e.what());
                // Revert assignment
                m_db->failChunk(chunk.job_id, chunk.frame_start, chunk.frame_end, 999);
            }
        }
        else
        {
            // HTTP POST to worker
            auto [host, port] = parseEndpoint(peer.endpoint);
            if (host.empty())
            {
                MonitorLog::instance().error("dispatch",
                    "Invalid endpoint for " + peer.node_id + ": " + peer.endpoint);
                // Revert: set back to pending
                m_db->failChunk(chunk.job_id, chunk.frame_start, chunk.frame_end, 999);
                continue;
            }

            try
            {
                httplib::Client cli(host, port);
                cli.set_connection_timeout(0, 500000); // 500ms — LAN should respond instantly
                cli.set_read_timeout(1);

                nlohmann::json body = {
                    {"manifest", nlohmann::json::parse(manifestJson)},
                    {"frame_start", chunk.frame_start},
                    {"frame_end", chunk.frame_end},
                };

                auto res = cli.Post("/api/dispatch/assign", body.dump(), "application/json");
                if (!res || res->status != 200)
                {
                    int status = res ? res->status : 0;
                    MonitorLog::instance().warn("dispatch",
                        "Assignment POST failed to " + peer.node_id +
                        " (status=" + std::to_string(status) + "), reverting to pending");
                    // Revert: set chunk back to pending immediately
                    m_db->failChunk(chunk.job_id, chunk.frame_start, chunk.frame_end, 999);
                }
                else
                {
                    MonitorLog::instance().info("dispatch",
                        "Assigned to " + peer.node_id + ": " + chunk.job_id +
                        " f" + std::to_string(chunk.frame_start) +
                        "-" + std::to_string(chunk.frame_end));
                }
            }
            catch (const std::exception& e)
            {
                MonitorLog::instance().error("dispatch",
                    std::string("HTTP POST error to ") + peer.node_id + ": " + e.what());
                // Revert
                m_db->failChunk(chunk.job_id, chunk.frame_start, chunk.frame_end, 999);
            }
        }
    }
}

bool DispatchManager::retryFailedChunks(const std::string& jobId)
{
    if (!m_db || !m_db->isOpen())
        return false;

    return m_db->retryFailedChunks(jobId);
}

std::string DispatchManager::resubmitJob(const std::string& sourceJobId)
{
    if (!m_db || !m_db->isOpen())
        return {};

    auto jobOpt = m_db->getJob(sourceJobId);
    if (!jobOpt.has_value())
        return {};

    try
    {
        auto manifest = nlohmann::json::parse(jobOpt->manifest_json).get<JobManifest>();

        // Generate new job_id: append "-v2", "-v3", etc.
        std::string baseSlug = manifest.job_id;
        // Strip existing -vN suffix
        auto vpos = baseSlug.rfind("-v");
        if (vpos != std::string::npos)
        {
            bool allDigits = true;
            for (size_t i = vpos + 2; i < baseSlug.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(baseSlug[i])))
                {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits && vpos + 2 < baseSlug.size())
                baseSlug = baseSlug.substr(0, vpos);
        }

        // Find next available suffix
        std::string newJobId;
        for (int suffix = 2; suffix < 1000; ++suffix)
        {
            newJobId = baseSlug + "-v" + std::to_string(suffix);
            if (!m_db->getJob(newJobId).has_value())
                break;
        }

        // Update manifest with new job_id and fresh timestamp
        manifest.job_id = newJobId;
        manifest.submitted_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Submit as new job (fresh chunks, zero retry counts, empty failed_on)
        return submitJob(manifest, jobOpt->priority);
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("dispatch",
            std::string("resubmitJob failed: ") + e.what());
        return {};
    }
}

void DispatchManager::doSnapshot()
{
    if (!m_app || !m_db || !m_db->isOpen())
        return;

    // Snapshot to a local temp file first (fast), then move to network FS on
    // a background thread so we never block the main thread on network I/O.
    auto localTmp = m_app->farmPath().parent_path() / "snapshot_tmp.db";
    if (!m_db->snapshotTo(localTmp))
        return;

    auto snapshotPath = m_app->farmPath() / "state" / "snapshot.db";
    std::thread([localTmp, snapshotPath]()
    {
        std::error_code ec;
        std::filesystem::copy_file(localTmp, snapshotPath,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(localTmp, ec);
        if (!ec)
            MonitorLog::instance().info("dispatch", "DB snapshot written");
        else
            MonitorLog::instance().warn("dispatch", "Snapshot copy failed: " + ec.message());
    }).detach();
}

} // namespace MR
