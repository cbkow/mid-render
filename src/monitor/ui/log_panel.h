#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace MR {

class MonitorApp;

struct TaskOutputLine
{
    std::string text;
    bool isHeader = false;
};

class LogPanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    void renderMonitorLog();
    void renderTaskOutput();
    void scanTaskOutput();

    MonitorApp* m_app = nullptr;
    bool m_autoScroll = true;

    // Mode
    enum class Mode { MonitorLog, TaskOutput };
    Mode m_mode = Mode::MonitorLog;

    // Task output cache
    std::string m_taskOutputJobId;
    std::vector<TaskOutputLine> m_taskOutputLines;
    std::chrono::steady_clock::time_point m_lastTaskOutputScan;
};

} // namespace MR
