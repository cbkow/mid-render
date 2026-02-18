#include "monitor/ui/log_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"
#include "core/monitor_log.h"

#include <imgui.h>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace MR {

void LogPanel::init(MonitorApp* app)
{
    m_app = app;
}

void LogPanel::render()
{
    if (!visible) return;

    if (ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Log", visible);

        // Dual-mode dropdown
        {
            std::string selectedJobId = m_app ? m_app->selectedJobId() : "";

            // Build current label
            std::string comboLabel;
            if (m_mode == Mode::MonitorLog)
                comboLabel = "Monitor Log";
            else if (!selectedJobId.empty())
                comboLabel = "Task Output: " + selectedJobId;
            else
                comboLabel = "Task Output";

            ImGui::SetNextItemWidth(300.0f);
            if (ImGui::BeginCombo("##LogMode", comboLabel.c_str()))
            {
                if (ImGui::Selectable("Monitor Log", m_mode == Mode::MonitorLog))
                    m_mode = Mode::MonitorLog;

                // Task Output option — always shown, label varies
                std::string taskLabel = selectedJobId.empty()
                    ? "Task Output"
                    : ("Task Output: " + selectedJobId);
                if (ImGui::Selectable(taskLabel.c_str(), m_mode == Mode::TaskOutput))
                    m_mode = Mode::TaskOutput;

                ImGui::EndCombo();
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);
        ImGui::Separator();

        switch (m_mode)
        {
            case Mode::MonitorLog: renderMonitorLog(); break;
            case Mode::TaskOutput: renderTaskOutput(); break;
        }
    }
    ImGui::End();
}

void LogPanel::renderMonitorLog()
{
    auto entries = MonitorLog::instance().getEntries();

    ImGui::BeginChild("LogScroll", ImVec2(0, 0), ImGuiChildFlags_None);

    if (Fonts::mono)
        ImGui::PushFont(Fonts::mono);

    for (const auto& entry : entries)
    {
        ImVec4 col(1, 1, 1, 1);
        if (entry.level == "WARN")
            col = ImVec4(1.0f, 0.85f, 0.0f, 1.0f);
        else if (entry.level == "ERROR")
            col = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        else
            col = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);

        ImGui::TextColored(col, "[%s] [%s] %s",
            entry.level.c_str(),
            entry.category.c_str(),
            entry.message.c_str());
    }

    if (Fonts::mono)
        ImGui::PopFont();

    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

void LogPanel::renderTaskOutput()
{
    if (!m_app)
    {
        ImGui::TextDisabled("No app context");
        return;
    }

    std::string jobId = m_app->selectedJobId();
    if (jobId.empty())
    {
        ImGui::TextDisabled("No job selected");
        return;
    }

    // Refresh on cooldown
    auto now = std::chrono::steady_clock::now();
    bool needScan = (jobId != m_taskOutputJobId);
    if (!needScan)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastTaskOutputScan).count();
        needScan = (elapsed >= 3000);
    }
    if (needScan)
        scanTaskOutput();

    if (m_taskOutputLines.empty())
    {
        ImGui::TextDisabled("No task output available");
        return;
    }

    ImGui::BeginChild("TaskOutputScroll", ImVec2(0, 0), ImGuiChildFlags_None);

    if (Fonts::mono)
        ImGui::PushFont(Fonts::mono);

    for (const auto& line : m_taskOutputLines)
    {
        if (line.isHeader)
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", line.text.c_str());
        else
            ImGui::TextUnformatted(line.text.c_str());
    }

    if (Fonts::mono)
        ImGui::PopFont();

    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

void LogPanel::scanTaskOutput()
{
    m_lastTaskOutputScan = std::chrono::steady_clock::now();
    m_taskOutputLines.clear();

    if (!m_app) return;

    std::string jobId = m_app->selectedJobId();
    m_taskOutputJobId = jobId;
    if (jobId.empty()) return;

    auto stdoutDir = m_app->farmPath() / "jobs" / jobId / "stdout";
    std::error_code ec;
    if (!std::filesystem::is_directory(stdoutDir, ec))
        return;

    // Collect all log files: {nodeId, rangeStr, timestampMs, path}
    struct LogFile
    {
        std::string nodeId;
        std::string rangeStr;
        int64_t timestampMs = 0;
        std::filesystem::path path;
    };
    std::vector<LogFile> logFiles;

    for (const auto& nodeEntry : std::filesystem::directory_iterator(stdoutDir, ec))
    {
        if (!nodeEntry.is_directory()) continue;
        std::string nodeId = nodeEntry.path().filename().string();

        for (const auto& fileEntry : std::filesystem::directory_iterator(nodeEntry.path(), ec))
        {
            if (!fileEntry.is_regular_file()) continue;
            std::string fname = fileEntry.path().filename().string();
            if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".log")
                continue;

            // Parse: {rangeStr}_{timestamp_ms}.log
            std::string stem = fname.substr(0, fname.size() - 4); // strip .log
            auto underPos = stem.rfind('_');
            if (underPos == std::string::npos) continue;

            LogFile lf;
            lf.nodeId = nodeId;
            lf.rangeStr = stem.substr(0, underPos);
            try { lf.timestampMs = std::stoll(stem.substr(underPos + 1)); }
            catch (...) { continue; }
            lf.path = fileEntry.path();
            logFiles.push_back(std::move(lf));
        }
    }

    // Sort by rangeStr then timestamp
    std::sort(logFiles.begin(), logFiles.end(), [](const LogFile& a, const LogFile& b)
    {
        if (a.rangeStr != b.rangeStr) return a.rangeStr < b.rangeStr;
        return a.timestampMs < b.timestampMs;
    });

    // Build output lines
    for (const auto& lf : logFiles)
    {
        // Format timestamp as HH:MM:SS
        time_t t = static_cast<time_t>(lf.timestampMs / 1000);
        struct tm tmBuf;
#ifdef _WIN32
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        char timeBuf[16];
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf);

        // Header line
        std::string header = lf.nodeId + "  |  f" + lf.rangeStr + "  |  " + timeBuf;
        m_taskOutputLines.push_back({header, true});

        // Read file contents
        std::ifstream ifs(lf.path);
        if (ifs.is_open())
        {
            std::string line;
            while (std::getline(ifs, line))
                m_taskOutputLines.push_back({std::move(line), false});
        }

        // Blank separator
        m_taskOutputLines.push_back({"", false});
    }
}

} // namespace MR
