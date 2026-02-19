#pragma once

#include "core/job_types.h"
#include "monitor/database_manager.h"

#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <mutex>

namespace MR {

class MonitorApp; // forward

enum class DetailMode { Empty, Submission, Detail };

class JobDetailPanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    void renderSubmissionMode();
    void renderDetailMode();
    void renderFrameGrid(const std::vector<ChunkRow>& chunks, int fStart, int fEnd);
    void onTemplateSelected(int idx);
    void doSubmit();
    void resolveOutputPatterns();

    MonitorApp* m_app = nullptr;
    DetailMode m_mode = DetailMode::Empty;

    // Submission state
    int m_selectedTemplateIdx = -1;
    char m_jobNameBuf[256] = {};
    char m_cmdPathBuf[512] = {};
    std::vector<std::array<char, 512>> m_flagBufs;

    struct OutputBuf
    {
        int flagIdx = -1;
        std::array<char, 512> dirBuf = {};
        std::array<char, 256> filenameBuf = {};
        bool overridden = false;
    };
    std::vector<OutputBuf> m_outputBufs;

    int m_frameStart = 1, m_frameEnd = 250, m_chunkSize = 1;
    int m_priority = 50, m_maxRetries = 3, m_timeout = 0;
    bool m_hasTimeout = false;
    std::vector<std::string> m_errors;

    // Detail state
    std::string m_detailJobId;
    JobInfo m_cachedDetail;
    bool m_hasDetailCache = false;
    bool m_pendingCancel = false, m_pendingDelete = false;
    bool m_pendingResubmit = false, m_pendingRetryFailed = false;

    // Chunk cache for detail view (refreshed every 3s for active jobs)
    std::vector<ChunkRow> m_detailChunks;
    std::string m_detailChunksJobId;
    std::string m_detailChunksLastState;  // detect state transitions for final refresh
    std::chrono::steady_clock::time_point m_lastChunkRefresh;

    // Async submission state (worker → leader)
    bool m_asyncSubmitting = false;
    std::string m_asyncSubmitSlug;
    std::mutex m_asyncResultMutex;
    int m_asyncResult = 0;  // 0=pending, 1=success, -1=fail
    std::string m_asyncResultError;
};

} // namespace MR
