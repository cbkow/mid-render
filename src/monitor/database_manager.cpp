#include "monitor/database_manager.h"
#include "core/monitor_log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>

namespace MR {

bool DatabaseManager::open(const std::filesystem::path& dbPath)
{
    try
    {
        std::error_code ec;
        std::filesystem::create_directories(dbPath.parent_path(), ec);

        m_db = std::make_unique<SQLite::Database>(
            dbPath.string(),
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        m_db->exec("PRAGMA journal_mode=WAL");
        m_db->exec("PRAGMA foreign_keys=ON");

        createSchema();

        MonitorLog::instance().info("db", "Database opened: " + dbPath.string());
        return true;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("Failed to open database: ") + e.what());
        m_db.reset();
        return false;
    }
}

void DatabaseManager::close()
{
    m_db.reset();
}

void DatabaseManager::createSchema()
{
    m_db->exec(R"(
        CREATE TABLE IF NOT EXISTS jobs (
            job_id TEXT PRIMARY KEY,
            manifest_json TEXT NOT NULL,
            current_state TEXT NOT NULL DEFAULT 'active',
            priority INTEGER NOT NULL DEFAULT 50,
            submitted_at_ms INTEGER NOT NULL
        )
    )");

    m_db->exec(R"(
        CREATE TABLE IF NOT EXISTS chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            job_id TEXT NOT NULL REFERENCES jobs(job_id) ON DELETE CASCADE,
            frame_start INTEGER NOT NULL,
            frame_end INTEGER NOT NULL,
            state TEXT NOT NULL DEFAULT 'pending',
            assigned_to TEXT,
            assigned_at_ms INTEGER,
            completed_at_ms INTEGER,
            retry_count INTEGER NOT NULL DEFAULT 0,
            completed_frames TEXT NOT NULL DEFAULT '[]',
            failed_on TEXT NOT NULL DEFAULT '[]'
        )
    )");

    m_db->exec("CREATE INDEX IF NOT EXISTS idx_chunks_job ON chunks(job_id)");
    m_db->exec("CREATE INDEX IF NOT EXISTS idx_chunks_state ON chunks(state)");

    // Migration: add completed_frames column (idempotent)
    try
    {
        m_db->exec("ALTER TABLE chunks ADD COLUMN completed_frames TEXT NOT NULL DEFAULT '[]'");
    }
    catch (...) {} // column already exists — safe to ignore

    // Migration: add failed_on column (idempotent)
    try
    {
        m_db->exec("ALTER TABLE chunks ADD COLUMN failed_on TEXT NOT NULL DEFAULT '[]'");
    }
    catch (...) {} // column already exists — safe to ignore
}

// --- Jobs ---

bool DatabaseManager::insertJob(const JobRow& job)
{
    try
    {
        SQLite::Statement q(*m_db,
            "INSERT INTO jobs (job_id, manifest_json, current_state, priority, submitted_at_ms) "
            "VALUES (?, ?, ?, ?, ?)");
        q.bind(1, job.job_id);
        q.bind(2, job.manifest_json);
        q.bind(3, job.current_state);
        q.bind(4, job.priority);
        q.bind(5, job.submitted_at_ms);
        q.exec();
        return true;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("insertJob failed: ") + e.what());
        return false;
    }
}

std::optional<JobRow> DatabaseManager::getJob(const std::string& jobId)
{
    try
    {
        SQLite::Statement q(*m_db,
            "SELECT job_id, manifest_json, current_state, priority, submitted_at_ms "
            "FROM jobs WHERE job_id = ?");
        q.bind(1, jobId);

        if (q.executeStep())
        {
            JobRow row;
            row.job_id = q.getColumn(0).getString();
            row.manifest_json = q.getColumn(1).getString();
            row.current_state = q.getColumn(2).getString();
            row.priority = q.getColumn(3).getInt();
            row.submitted_at_ms = q.getColumn(4).getInt64();
            return row;
        }
        return std::nullopt;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("getJob failed: ") + e.what());
        return std::nullopt;
    }
}

std::vector<JobSummary> DatabaseManager::getAllJobs()
{
    std::vector<JobSummary> result;
    try
    {
        // Get all jobs
        SQLite::Statement q(*m_db,
            "SELECT job_id, manifest_json, current_state, priority, submitted_at_ms "
            "FROM jobs ORDER BY priority ASC, submitted_at_ms ASC");

        while (q.executeStep())
        {
            JobSummary s;
            s.job.job_id = q.getColumn(0).getString();
            s.job.manifest_json = q.getColumn(1).getString();
            s.job.current_state = q.getColumn(2).getString();
            s.job.priority = q.getColumn(3).getInt();
            s.job.submitted_at_ms = q.getColumn(4).getInt64();

            // Get chunk progress
            SQLite::Statement cq(*m_db,
                "SELECT state, COUNT(*) FROM chunks WHERE job_id = ? GROUP BY state");
            cq.bind(1, s.job.job_id);
            while (cq.executeStep())
            {
                std::string state = cq.getColumn(0).getString();
                int count = cq.getColumn(1).getInt();
                s.progress.total += count;
                if (state == "completed")      s.progress.completed = count;
                else if (state == "failed")    s.progress.failed = count;
                else if (state == "assigned")  s.progress.rendering = count;
                else if (state == "pending")   s.progress.pending = count;
            }

            result.push_back(std::move(s));
        }
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("getAllJobs failed: ") + e.what());
    }
    return result;
}

bool DatabaseManager::updateJobState(const std::string& jobId, const std::string& newState)
{
    try
    {
        SQLite::Statement q(*m_db, "UPDATE jobs SET current_state = ? WHERE job_id = ?");
        q.bind(1, newState);
        q.bind(2, jobId);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("updateJobState failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::updateJobPriority(const std::string& jobId, int priority)
{
    try
    {
        SQLite::Statement q(*m_db, "UPDATE jobs SET priority = ? WHERE job_id = ?");
        q.bind(1, priority);
        q.bind(2, jobId);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("updateJobPriority failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::deleteJob(const std::string& jobId)
{
    try
    {
        SQLite::Statement q(*m_db, "DELETE FROM jobs WHERE job_id = ?");
        q.bind(1, jobId);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("deleteJob failed: ") + e.what());
        return false;
    }
}

// --- Chunks ---

bool DatabaseManager::insertChunks(const std::string& jobId, const std::vector<ChunkRange>& chunks)
{
    try
    {
        SQLite::Transaction transaction(*m_db);
        SQLite::Statement q(*m_db,
            "INSERT INTO chunks (job_id, frame_start, frame_end) VALUES (?, ?, ?)");

        for (const auto& c : chunks)
        {
            q.bind(1, jobId);
            q.bind(2, c.frame_start);
            q.bind(3, c.frame_end);
            q.exec();
            q.reset();
        }

        transaction.commit();
        return true;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("insertChunks failed: ") + e.what());
        return false;
    }
}

std::vector<ChunkRow> DatabaseManager::getChunksForJob(const std::string& jobId)
{
    std::vector<ChunkRow> result;
    try
    {
        SQLite::Statement q(*m_db,
            "SELECT id, job_id, frame_start, frame_end, state, assigned_to, "
            "assigned_at_ms, completed_at_ms, retry_count, completed_frames, failed_on "
            "FROM chunks WHERE job_id = ? ORDER BY frame_start ASC");
        q.bind(1, jobId);

        while (q.executeStep())
        {
            ChunkRow row;
            row.id = q.getColumn(0).getInt64();
            row.job_id = q.getColumn(1).getString();
            row.frame_start = q.getColumn(2).getInt();
            row.frame_end = q.getColumn(3).getInt();
            row.state = q.getColumn(4).getString();
            row.assigned_to = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
            row.assigned_at_ms = q.getColumn(6).isNull() ? 0 : q.getColumn(6).getInt64();
            row.completed_at_ms = q.getColumn(7).isNull() ? 0 : q.getColumn(7).getInt64();
            row.retry_count = q.getColumn(8).getInt();

            // Parse completed_frames JSON array
            std::string cfJson = q.getColumn(9).getString();
            try
            {
                auto arr = nlohmann::json::parse(cfJson);
                if (arr.is_array())
                    row.completed_frames = arr.get<std::vector<int>>();
            }
            catch (...) {}

            // Parse failed_on JSON array
            std::string foJson = q.getColumn(10).getString();
            try
            {
                auto arr = nlohmann::json::parse(foJson);
                if (arr.is_array())
                    row.failed_on = arr.get<std::vector<std::string>>();
            }
            catch (...) {}

            result.push_back(std::move(row));
        }
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("getChunksForJob failed: ") + e.what());
    }
    return result;
}

// --- Dispatch operations ---

std::optional<std::pair<ChunkRow, std::string>>
DatabaseManager::findNextPendingChunk()
{
    try
    {
        SQLite::Statement q(*m_db, R"(
            SELECT c.id, c.job_id, c.frame_start, c.frame_end, j.manifest_json
            FROM chunks c
            JOIN jobs j ON c.job_id = j.job_id
            WHERE c.state = 'pending' AND j.current_state = 'active'
            ORDER BY j.priority ASC, j.submitted_at_ms ASC, c.frame_start ASC
            LIMIT 1
        )");

        if (q.executeStep())
        {
            ChunkRow row;
            row.id = q.getColumn(0).getInt64();
            row.job_id = q.getColumn(1).getString();
            row.frame_start = q.getColumn(2).getInt();
            row.frame_end = q.getColumn(3).getInt();
            row.state = "pending";

            std::string manifestJson = q.getColumn(4).getString();
            return std::make_pair(std::move(row), std::move(manifestJson));
        }
        return std::nullopt;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("findNextPendingChunk failed: ") + e.what());
        return std::nullopt;
    }
}

std::optional<std::pair<ChunkRow, std::string>>
DatabaseManager::findNextPendingChunkForNode(const std::vector<std::string>& nodeTags,
                                              const std::string& nodeId)
{
    try
    {
        // First: find active jobs with pending work, ordered by priority
        SQLite::Statement jobQ(*m_db, R"(
            SELECT j.job_id, j.manifest_json
            FROM jobs j
            WHERE j.current_state = 'active'
              AND EXISTS (SELECT 1 FROM chunks c WHERE c.job_id = j.job_id AND c.state = 'pending')
            ORDER BY j.priority ASC, j.submitted_at_ms ASC
        )");

        while (jobQ.executeStep())
        {
            std::string jobId = jobQ.getColumn(0).getString();
            std::string manifestJson = jobQ.getColumn(1).getString();

            // Check tags_required from manifest against node's tags
            try
            {
                auto mj = nlohmann::json::parse(manifestJson);
                auto tagsReq = mj.value("tags_required", std::vector<std::string>{});
                bool eligible = true;
                for (const auto& req : tagsReq)
                {
                    if (std::find(nodeTags.begin(), nodeTags.end(), req) == nodeTags.end())
                    {
                        eligible = false;
                        break;
                    }
                }
                if (!eligible)
                    continue;
            }
            catch (...)
            {
                continue; // malformed manifest — skip job
            }

            // Job is eligible — find its first pending chunk not blacklisted for this node
            SQLite::Statement chunkQ(*m_db,
                "SELECT id, frame_start, frame_end, failed_on FROM chunks "
                "WHERE job_id = ? AND state = 'pending' "
                "ORDER BY frame_start ASC");
            chunkQ.bind(1, jobId);

            while (chunkQ.executeStep())
            {
                // Check blacklist: skip if this node is in failed_on
                if (!nodeId.empty())
                {
                    std::string foJson = chunkQ.getColumn(3).getString();
                    try
                    {
                        auto failedOn = nlohmann::json::parse(foJson).get<std::vector<std::string>>();
                        if (std::find(failedOn.begin(), failedOn.end(), nodeId) != failedOn.end())
                        {
                            continue; // blacklisted — try next chunk
                        }
                    }
                    catch (...) {}
                }

                ChunkRow row;
                row.id = chunkQ.getColumn(0).getInt64();
                row.job_id = jobId;
                row.frame_start = chunkQ.getColumn(1).getInt();
                row.frame_end = chunkQ.getColumn(2).getInt();
                row.state = "pending";
                return std::make_pair(std::move(row), std::move(manifestJson));
            }
        }
        return std::nullopt;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db",
            std::string("findNextPendingChunkForNode failed: ") + e.what());
        return std::nullopt;
    }
}

bool DatabaseManager::assignChunk(int64_t chunkId, const std::string& nodeId, int64_t nowMs)
{
    try
    {
        SQLite::Statement q(*m_db,
            "UPDATE chunks SET state = 'assigned', assigned_to = ?, assigned_at_ms = ? "
            "WHERE id = ? AND state = 'pending'");
        q.bind(1, nodeId);
        q.bind(2, nowMs);
        q.bind(3, chunkId);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("assignChunk failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::completeChunk(const std::string& jobId, int frameStart, int frameEnd, int64_t nowMs)
{
    try
    {
        SQLite::Statement q(*m_db,
            "UPDATE chunks SET state = 'completed', completed_at_ms = ? "
            "WHERE job_id = ? AND frame_start = ? AND frame_end = ? AND state = 'assigned'");
        q.bind(1, nowMs);
        q.bind(2, jobId);
        q.bind(3, frameStart);
        q.bind(4, frameEnd);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("completeChunk failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::failChunk(const std::string& jobId, int frameStart, int frameEnd,
                                int maxRetries, const std::string& failingNodeId)
{
    try
    {
        // First, append failingNodeId to failed_on if provided and not already present
        if (!failingNodeId.empty())
        {
            SQLite::Statement selQ(*m_db,
                "SELECT id, failed_on FROM chunks "
                "WHERE job_id = ? AND frame_start = ? AND frame_end = ? LIMIT 1");
            selQ.bind(1, jobId);
            selQ.bind(2, frameStart);
            selQ.bind(3, frameEnd);

            if (selQ.executeStep())
            {
                int64_t chunkId = selQ.getColumn(0).getInt64();
                std::string foJson = selQ.getColumn(1).getString();

                std::vector<std::string> failedOn;
                try { failedOn = nlohmann::json::parse(foJson).get<std::vector<std::string>>(); }
                catch (...) {}

                if (std::find(failedOn.begin(), failedOn.end(), failingNodeId) == failedOn.end())
                {
                    failedOn.push_back(failingNodeId);
                    SQLite::Statement upd(*m_db,
                        "UPDATE chunks SET failed_on = ? WHERE id = ?");
                    upd.bind(1, nlohmann::json(failedOn).dump());
                    upd.bind(2, chunkId);
                    upd.exec();
                }
            }
        }

        // Increment retry_count. If under max retries, reset to pending for retry.
        SQLite::Statement q(*m_db,
            "UPDATE chunks SET "
            "  retry_count = retry_count + 1, "
            "  state = CASE WHEN retry_count + 1 < ? THEN 'pending' ELSE 'failed' END, "
            "  assigned_to = CASE WHEN retry_count + 1 < ? THEN NULL ELSE assigned_to END, "
            "  assigned_at_ms = CASE WHEN retry_count + 1 < ? THEN NULL ELSE assigned_at_ms END "
            "WHERE job_id = ? AND frame_start = ? AND frame_end = ?");
        q.bind(1, maxRetries);
        q.bind(2, maxRetries);
        q.bind(3, maxRetries);
        q.bind(4, jobId);
        q.bind(5, frameStart);
        q.bind(6, frameEnd);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("failChunk failed: ") + e.what());
        return false;
    }
}

int DatabaseManager::reassignDeadWorkerChunks(const std::string& deadNodeId)
{
    try
    {
        SQLite::Statement q(*m_db,
            "UPDATE chunks SET state = 'pending', assigned_to = NULL, assigned_at_ms = NULL "
            "WHERE assigned_to = ? AND state = 'assigned'");
        q.bind(1, deadNodeId);
        int count = q.exec();
        if (count > 0)
            MonitorLog::instance().info("db", "Reassigned " + std::to_string(count) +
                " chunks from dead worker " + deadNodeId);
        return count;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("reassignDeadWorkerChunks failed: ") + e.what());
        return 0;
    }
}

bool DatabaseManager::isJobComplete(const std::string& jobId)
{
    try
    {
        SQLite::Statement q(*m_db,
            "SELECT COUNT(*) FROM chunks WHERE job_id = ? AND state NOT IN ('completed', 'failed')");
        q.bind(1, jobId);
        if (q.executeStep())
            return q.getColumn(0).getInt() == 0;
        return false;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("isJobComplete failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::resetAllChunks(const std::string& jobId)
{
    try
    {
        SQLite::Statement q(*m_db,
            "UPDATE chunks SET state = 'pending', assigned_to = NULL, "
            "assigned_at_ms = NULL, completed_at_ms = NULL, retry_count = 0, "
            "completed_frames = '[]', failed_on = '[]' "
            "WHERE job_id = ?");
        q.bind(1, jobId);
        return q.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("resetAllChunks failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::retryFailedChunks(const std::string& jobId)
{
    try
    {
        // Reset only failed chunks to pending. Keep failed_on (blacklist persists).
        SQLite::Statement q(*m_db,
            "UPDATE chunks SET state = 'pending', assigned_to = NULL, "
            "assigned_at_ms = NULL, retry_count = 0, completed_frames = '[]' "
            "WHERE job_id = ? AND state = 'failed'");
        q.bind(1, jobId);
        int count = q.exec();

        if (count > 0)
        {
            // Ensure job state is active so dispatch picks up the retried chunks
            updateJobState(jobId, "active");
            MonitorLog::instance().info("db",
                "Retried " + std::to_string(count) + " failed chunks for " + jobId);
        }
        return count > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("retryFailedChunks failed: ") + e.what());
        return false;
    }
}

// --- Per-frame completion tracking ---

bool DatabaseManager::addCompletedFrames(const std::string& jobId, int frame)
{
    try
    {
        // Find the chunk that contains this frame
        SQLite::Statement q(*m_db,
            "SELECT id, completed_frames FROM chunks "
            "WHERE job_id = ? AND frame_start <= ? AND frame_end >= ? "
            "LIMIT 1");
        q.bind(1, jobId);
        q.bind(2, frame);
        q.bind(3, frame);

        if (!q.executeStep())
            return false;

        int64_t chunkId = q.getColumn(0).getInt64();
        std::string cfJson = q.getColumn(1).getString();

        // Parse, insert, write back
        std::vector<int> cf;
        try { cf = nlohmann::json::parse(cfJson).get<std::vector<int>>(); }
        catch (...) {}

        if (std::find(cf.begin(), cf.end(), frame) == cf.end())
        {
            cf.push_back(frame);
            std::sort(cf.begin(), cf.end());
        }

        SQLite::Statement upd(*m_db,
            "UPDATE chunks SET completed_frames = ? WHERE id = ?");
        upd.bind(1, nlohmann::json(cf).dump());
        upd.bind(2, chunkId);
        return upd.exec() > 0;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("addCompletedFrames failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::addCompletedFramesBatch(const std::string& jobId, const std::vector<int>& frames)
{
    if (frames.empty()) return true;

    try
    {
        SQLite::Transaction transaction(*m_db);

        // Load all chunks for this job
        SQLite::Statement q(*m_db,
            "SELECT id, frame_start, frame_end, completed_frames FROM chunks "
            "WHERE job_id = ? ORDER BY frame_start ASC");
        q.bind(1, jobId);

        struct ChunkData { int64_t id; int fs, fe; std::vector<int> cf; bool dirty = false; };
        std::vector<ChunkData> chunkData;
        while (q.executeStep())
        {
            ChunkData cd;
            cd.id = q.getColumn(0).getInt64();
            cd.fs = q.getColumn(1).getInt();
            cd.fe = q.getColumn(2).getInt();
            try { cd.cf = nlohmann::json::parse(q.getColumn(3).getString()).get<std::vector<int>>(); }
            catch (...) {}
            chunkData.push_back(std::move(cd));
        }

        // Assign each frame to its chunk
        for (int frame : frames)
        {
            for (auto& cd : chunkData)
            {
                if (frame >= cd.fs && frame <= cd.fe)
                {
                    if (std::find(cd.cf.begin(), cd.cf.end(), frame) == cd.cf.end())
                    {
                        cd.cf.push_back(frame);
                        cd.dirty = true;
                    }
                    break;
                }
            }
        }

        // Write back dirty chunks
        SQLite::Statement upd(*m_db,
            "UPDATE chunks SET completed_frames = ? WHERE id = ?");
        for (auto& cd : chunkData)
        {
            if (!cd.dirty) continue;
            std::sort(cd.cf.begin(), cd.cf.end());
            upd.bind(1, nlohmann::json(cd.cf).dump());
            upd.bind(2, cd.id);
            upd.exec();
            upd.reset();
        }

        transaction.commit();
        return true;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("addCompletedFramesBatch failed: ") + e.what());
        return false;
    }
}

// --- Snapshot ---

bool DatabaseManager::snapshotTo(const std::filesystem::path& destPath)
{
    if (!m_db) return false;

    try
    {
        std::error_code ec;
        std::filesystem::create_directories(destPath.parent_path(), ec);

        // Use SQLite backup API
        SQLite::Database destDb(destPath.string(),
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        SQLite::Backup backup(destDb, *m_db);
        backup.executeStep(-1);  // copy entire DB in one step
        return true;
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("snapshotTo failed: ") + e.what());
        return false;
    }
}

bool DatabaseManager::restoreFrom(const std::filesystem::path& snapshotPath,
                                   const std::filesystem::path& localPath)
{
    std::error_code ec;
    if (!std::filesystem::exists(snapshotPath, ec))
        return false;

    try
    {
        // Copy snapshot file to local path, then open it
        std::filesystem::create_directories(localPath.parent_path(), ec);
        std::filesystem::copy_file(snapshotPath, localPath,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            MonitorLog::instance().error("db", "Failed to copy snapshot: " + ec.message());
            return false;
        }

        return open(localPath);
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("db", std::string("restoreFrom failed: ") + e.what());
        return false;
    }
}

} // namespace MR
