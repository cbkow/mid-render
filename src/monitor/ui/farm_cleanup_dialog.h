#pragma once

#include <string>
#include <vector>

namespace MR {

class MonitorApp;

class FarmCleanupDialog
{
public:
    void init(MonitorApp* app);
    void render();
    void open();

private:
    struct CleanupItem
    {
        std::string id;        // job_id, node_id, or dir path
        std::string label;
        std::string detail;
        bool selected = false;
    };

    void scanItems();
    void renderSection(const char* header, const char* actionLabel,
                       std::vector<CleanupItem>& items, bool enabled,
                       const char* disabledMsg = nullptr);
    void archiveSelected();
    void deleteArchivedSelected();
    void deleteOrphansSelected();
    void removePeersSelected();

    MonitorApp* m_app = nullptr;
    bool m_shouldOpen = false;
    bool m_hasScanned = false;

    // Section data
    std::vector<CleanupItem> m_finishedJobs;   // completed/cancelled -> archivable
    std::vector<CleanupItem> m_archivedJobs;   // archived -> permanently deletable
    std::vector<CleanupItem> m_orphanedDirs;   // job dirs on shared FS not in DB
    std::vector<CleanupItem> m_stalePeers;     // peers with is_alive == false
};

} // namespace MR
