#include "monitor/ui/farm_cleanup_dialog.h"
#include "monitor/monitor_app.h"
#include "monitor/database_manager.h"
#include "monitor/peer_manager.h"
#include "core/monitor_log.h"

#include <imgui.h>
#include <set>
#include <ctime>
#include <filesystem>

namespace MR {

namespace fs = std::filesystem;

void FarmCleanupDialog::init(MonitorApp* app)
{
    m_app = app;
}

void FarmCleanupDialog::open()
{
    m_shouldOpen = true;
    m_hasScanned = false;
}

static std::string formatTimestamp(int64_t ms)
{
    if (ms <= 0) return "unknown";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmBuf);
    return buf;
}

void FarmCleanupDialog::scanItems()
{
    m_finishedJobs.clear();
    m_archivedJobs.clear();
    m_orphanedDirs.clear();
    m_stalePeers.clear();

    if (!m_app || !m_app->isFarmRunning())
        return;

    // Section 1 & 2: Jobs from DB (leader only)
    if (m_app->isLeader() && m_app->databaseManager().isOpen())
    {
        auto allJobs = m_app->databaseManager().getAllJobs();
        std::set<std::string> dbJobIds;

        for (const auto& s : allJobs)
        {
            dbJobIds.insert(s.job.job_id);

            if (s.job.current_state == "completed" || s.job.current_state == "cancelled")
            {
                std::string detail = s.job.current_state;
                detail += " | " + std::to_string(s.progress.total) + " chunks";
                m_finishedJobs.push_back({
                    s.job.job_id,
                    s.job.job_id,
                    detail,
                    false
                });
            }
            else if (s.job.current_state == "archived")
            {
                m_archivedJobs.push_back({
                    s.job.job_id,
                    s.job.job_id,
                    "archived",
                    false
                });
            }
        }

        // Section 3: Orphaned dirs (dirs in jobs/ not matching any DB job)
        auto jobsDir = m_app->farmPath() / "jobs";
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(jobsDir, ec))
        {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            if (dbJobIds.find(dirName) == dbJobIds.end())
            {
                m_orphanedDirs.push_back({
                    entry.path().string(),
                    dirName,
                    "no matching DB entry",
                    false
                });
            }
        }
    }
    else
    {
        // Worker: use cached jobs for orphan detection
        std::set<std::string> knownIds;
        for (const auto& j : m_app->cachedJobs())
            knownIds.insert(j.manifest.job_id);

        auto jobsDir = m_app->farmPath() / "jobs";
        std::error_code ec;
        if (fs::is_directory(jobsDir, ec))
        {
            for (auto& entry : fs::directory_iterator(jobsDir, ec))
            {
                if (!entry.is_directory()) continue;
                std::string dirName = entry.path().filename().string();
                if (knownIds.find(dirName) == knownIds.end())
                {
                    m_orphanedDirs.push_back({
                        entry.path().string(),
                        dirName,
                        "not in job list",
                        false
                    });
                }
            }
        }
    }

    // Section 4: Stale peers (any node can see this)
    auto peers = m_app->peerManager().getPeerSnapshot();
    for (const auto& p : peers)
    {
        if (!p.is_alive && !p.is_local)
        {
            std::string detail = "last seen: " + formatTimestamp(p.last_seen_ms);
            m_stalePeers.push_back({
                p.node_id,
                p.hostname + " (" + p.node_id.substr(0, 8) + ")",
                detail,
                false
            });
        }
    }

    m_hasScanned = true;
}

void FarmCleanupDialog::renderSection(const char* header, const char* actionLabel,
                                       std::vector<CleanupItem>& items, bool enabled,
                                       const char* disabledMsg)
{
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Indent(8.0f);

    if (!enabled)
    {
        ImGui::TextDisabled("%s", disabledMsg ? disabledMsg : "Not available");
        ImGui::Unindent(8.0f);
        return;
    }

    if (items.empty())
    {
        ImGui::TextDisabled("None");
        ImGui::Unindent(8.0f);
        return;
    }

    // Select All
    bool allSelected = true;
    for (const auto& item : items)
        if (!item.selected) { allSelected = false; break; }

    std::string selAllId = std::string("Select All##") + header;
    if (ImGui::Checkbox(selAllId.c_str(), &allSelected))
    {
        for (auto& item : items)
            item.selected = allSelected;
    }

    // Items
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto& item = items[i];
        std::string cbId = item.label + "##" + std::to_string(i) + header;
        ImGui::Checkbox(cbId.c_str(), &item.selected);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", item.detail.c_str());
    }

    // Count selected
    int selectedCount = 0;
    for (const auto& item : items)
        if (item.selected) selectedCount++;

    // Action button
    ImGui::BeginDisabled(selectedCount == 0);
    std::string btnLabel = std::string(actionLabel) + " (" + std::to_string(selectedCount) + ")##" + header;
    if (ImGui::Button(btnLabel.c_str()))
    {
        // Caller handles the action
    }
    ImGui::EndDisabled();

    ImGui::Unindent(8.0f);
}

void FarmCleanupDialog::archiveSelected()
{
    for (auto it = m_finishedJobs.begin(); it != m_finishedJobs.end(); )
    {
        if (it->selected)
        {
            m_app->archiveJob(it->id);
            it = m_finishedJobs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FarmCleanupDialog::deleteArchivedSelected()
{
    for (auto it = m_archivedJobs.begin(); it != m_archivedJobs.end(); )
    {
        if (it->selected)
        {
            m_app->deleteJob(it->id);
            it = m_archivedJobs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FarmCleanupDialog::deleteOrphansSelected()
{
    for (auto it = m_orphanedDirs.begin(); it != m_orphanedDirs.end(); )
    {
        if (it->selected)
        {
            std::error_code ec;
            fs::remove_all(fs::path(it->id), ec);
            if (ec)
                MonitorLog::instance().warn("farm", "Failed to remove orphan: " + ec.message());
            else
                MonitorLog::instance().info("farm", "Removed orphan dir: " + it->label);
            it = m_orphanedDirs.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FarmCleanupDialog::removePeersSelected()
{
    for (auto it = m_stalePeers.begin(); it != m_stalePeers.end(); )
    {
        if (it->selected)
        {
            auto nodeDir = m_app->farmPath() / "nodes" / it->id;
            std::error_code ec;
            fs::remove_all(nodeDir, ec);
            if (ec)
                MonitorLog::instance().warn("farm", "Failed to remove peer dir: " + ec.message());
            else
                MonitorLog::instance().info("farm", "Removed stale peer: " + it->label);
            it = m_stalePeers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FarmCleanupDialog::render()
{
    if (m_shouldOpen)
    {
        ImGui::OpenPopup("Farm Cleanup");
        m_shouldOpen = false;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 modalSize(viewport->WorkSize.x * 0.9f, viewport->WorkSize.y * 0.9f);
    ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
    ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                  viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.09f, 0.09f, 0.09f, 1.0f));
    if (!ImGui::BeginPopupModal("Farm Cleanup", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleColor();

    if (!m_app || !m_app->isFarmRunning())
    {
        ImGui::TextDisabled("Farm not connected");
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    float buttonRowHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("CleanupContent", ImVec2(0, -buttonRowHeight), ImGuiChildFlags_None);

    // Scan button
    if (ImGui::Button("Scan"))
        scanItems();
    ImGui::SameLine();
    if (m_hasScanned)
        ImGui::Text("Found: %d finished, %d archived, %d orphaned, %d stale",
            (int)m_finishedJobs.size(), (int)m_archivedJobs.size(),
            (int)m_orphanedDirs.size(), (int)m_stalePeers.size());
    else
        ImGui::TextDisabled("Click Scan to search for cleanup items");

    ImGui::Separator();

    if (m_hasScanned)
    {

            bool isLeader = m_app->isLeader() && m_app->databaseManager().isOpen();

            // Section 1: Finished Jobs (archivable)
            {
                if (!ImGui::CollapsingHeader("Finished Jobs (Archivable)", ImGuiTreeNodeFlags_DefaultOpen))
                    goto section2;

                ImGui::Indent(8.0f);
                if (!isLeader)
                {
                    ImGui::TextDisabled("Available on leader only");
                    ImGui::Unindent(8.0f);
                    goto section2;
                }
                if (m_finishedJobs.empty())
                {
                    ImGui::TextDisabled("None");
                    ImGui::Unindent(8.0f);
                    goto section2;
                }

                {
                    bool allSel = true;
                    for (const auto& item : m_finishedJobs) if (!item.selected) { allSel = false; break; }
                    if (ImGui::Checkbox("Select All##finished", &allSel))
                        for (auto& item : m_finishedJobs) item.selected = allSel;
                }

                for (size_t i = 0; i < m_finishedJobs.size(); ++i)
                {
                    auto& item = m_finishedJobs[i];
                    std::string cbId = "##fin" + std::to_string(i);
                    ImGui::Checkbox(cbId.c_str(), &item.selected);
                    ImGui::SameLine();
                    ImGui::Text("%s", item.label.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", item.detail.c_str());
                }

                {
                    int cnt = 0;
                    for (const auto& it : m_finishedJobs) if (it.selected) cnt++;
                    ImGui::BeginDisabled(cnt == 0);
                    std::string btn = "Archive Selected (" + std::to_string(cnt) + ")##fin";
                    if (ImGui::Button(btn.c_str()))
                        archiveSelected();
                    ImGui::EndDisabled();
                }
                ImGui::Unindent(8.0f);
            }

section2:
            // Section 2: Archived Jobs (permanently deletable)
            {
                if (!ImGui::CollapsingHeader("Archived Jobs (Deletable)", ImGuiTreeNodeFlags_DefaultOpen))
                    goto section3;

                ImGui::Indent(8.0f);
                if (!isLeader)
                {
                    ImGui::TextDisabled("Available on leader only");
                    ImGui::Unindent(8.0f);
                    goto section3;
                }
                if (m_archivedJobs.empty())
                {
                    ImGui::TextDisabled("None");
                    ImGui::Unindent(8.0f);
                    goto section3;
                }

                {
                    bool allSel = true;
                    for (const auto& item : m_archivedJobs) if (!item.selected) { allSel = false; break; }
                    if (ImGui::Checkbox("Select All##archived", &allSel))
                        for (auto& item : m_archivedJobs) item.selected = allSel;
                }

                for (size_t i = 0; i < m_archivedJobs.size(); ++i)
                {
                    auto& item = m_archivedJobs[i];
                    std::string cbId = "##arch" + std::to_string(i);
                    ImGui::Checkbox(cbId.c_str(), &item.selected);
                    ImGui::SameLine();
                    ImGui::Text("%s", item.label.c_str());
                }

                {
                    int cnt = 0;
                    for (const auto& it : m_archivedJobs) if (it.selected) cnt++;
                    ImGui::BeginDisabled(cnt == 0);
                    std::string btn = "Delete Selected (" + std::to_string(cnt) + ")##arch";
                    if (ImGui::Button(btn.c_str()))
                        deleteArchivedSelected();
                    ImGui::EndDisabled();
                }
                ImGui::Unindent(8.0f);
            }

section3:
            // Section 3: Orphaned Directories
            {
                if (!ImGui::CollapsingHeader("Orphaned Directories", ImGuiTreeNodeFlags_DefaultOpen))
                    goto section4;

                ImGui::Indent(8.0f);
                if (m_orphanedDirs.empty())
                {
                    ImGui::TextDisabled("None");
                    ImGui::Unindent(8.0f);
                    goto section4;
                }

                {
                    bool allSel = true;
                    for (const auto& item : m_orphanedDirs) if (!item.selected) { allSel = false; break; }
                    if (ImGui::Checkbox("Select All##orphans", &allSel))
                        for (auto& item : m_orphanedDirs) item.selected = allSel;
                }

                for (size_t i = 0; i < m_orphanedDirs.size(); ++i)
                {
                    auto& item = m_orphanedDirs[i];
                    std::string cbId = "##orph" + std::to_string(i);
                    ImGui::Checkbox(cbId.c_str(), &item.selected);
                    ImGui::SameLine();
                    ImGui::Text("%s", item.label.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", item.detail.c_str());
                }

                {
                    int cnt = 0;
                    for (const auto& it : m_orphanedDirs) if (it.selected) cnt++;
                    ImGui::BeginDisabled(cnt == 0);
                    std::string btn = "Delete Selected (" + std::to_string(cnt) + ")##orph";
                    if (ImGui::Button(btn.c_str()))
                        deleteOrphansSelected();
                    ImGui::EndDisabled();
                }
                ImGui::Unindent(8.0f);
            }

section4:
            // Section 4: Stale Peers
            {
                if (!ImGui::CollapsingHeader("Stale Peers", ImGuiTreeNodeFlags_DefaultOpen))
                    goto sectionEnd;

                ImGui::Indent(8.0f);
                if (m_stalePeers.empty())
                {
                    ImGui::TextDisabled("None");
                    ImGui::Unindent(8.0f);
                    goto sectionEnd;
                }

                {
                    bool allSel = true;
                    for (const auto& item : m_stalePeers) if (!item.selected) { allSel = false; break; }
                    if (ImGui::Checkbox("Select All##peers", &allSel))
                        for (auto& item : m_stalePeers) item.selected = allSel;
                }

                for (size_t i = 0; i < m_stalePeers.size(); ++i)
                {
                    auto& item = m_stalePeers[i];
                    std::string cbId = "##peer" + std::to_string(i);
                    ImGui::Checkbox(cbId.c_str(), &item.selected);
                    ImGui::SameLine();
                    ImGui::Text("%s", item.label.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", item.detail.c_str());
                }

                {
                    int cnt = 0;
                    for (const auto& it : m_stalePeers) if (it.selected) cnt++;
                    ImGui::BeginDisabled(cnt == 0);
                    std::string btn = "Remove Selected (" + std::to_string(cnt) + ")##peers";
                    if (ImGui::Button(btn.c_str()))
                        removePeersSelected();
                    ImGui::EndDisabled();
                }
                ImGui::Unindent(8.0f);
            }

sectionEnd: ;
    }

    ImGui::EndChild(); // CleanupContent

    // --- Close button at bottom ---
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

} // namespace MR
