#pragma once

#include <chrono>
#include <filesystem>

namespace MR {

class MonitorApp;

class SubmissionWatcher
{
public:
    void init(MonitorApp* app, const std::filesystem::path& appDataDir);
    void poll();

private:
    void processSubmission(const std::filesystem::path& jsonPath);

    MonitorApp* m_app = nullptr;
    std::filesystem::path m_submissionsDir;
    std::chrono::steady_clock::time_point m_lastPoll;
};

} // namespace MR
