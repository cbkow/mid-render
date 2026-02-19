#include "monitor/ui/node_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"
#include "monitor/dispatch_manager.h"
#include "core/net_utils.h"

#include <imgui.h>
#include <httplib.h>
#include <algorithm>
#include <thread>

namespace MR {

void NodePanel::init(MonitorApp* app)
{
    m_app = app;
}

static void drawStatusBadge(const char* label, const ImVec4& color)
{
    ImGui::TextColored(color, "[%s]", label);
}

void NodePanel::render()
{
    if (!visible) return;

    if (ImGui::Begin("Node Overview", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Nodes", visible);
        if (!m_app || !m_app->isFarmRunning())
        {
            ImGui::TextDisabled("Farm not connected.");
            ImGui::TextDisabled("Configure Sync Root in Settings.");

            if (m_app && m_app->hasFarmError())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Error: %s",
                                   m_app->farmError().c_str());
            }
        }
        else
        {
            // --- Local node ---
            const auto& id = m_app->identity();
            ImGui::TextUnformatted("This Node");
            ImGui::SameLine();
            if (m_app->isLeader())
                drawStatusBadge("Leader", ImVec4(1.0f, 0.84f, 0.0f, 1.0f));

            ImGui::Spacing();
            ImGui::Text("ID: %s", id.nodeId().c_str());
            ImGui::Text("Host: %s", id.systemInfo().hostname.c_str());

            if (m_app->nodeState() == NodeState::Active)
            {
                if (m_app->renderCoordinator().isRendering())
                    drawStatusBadge("Rendering", ImVec4(0.3f, 0.5f, 1.0f, 1.0f));
                else
                    drawStatusBadge("Active", ImVec4(0.3f, 0.9f, 0.3f, 1.0f));
            }
            else
            {
                drawStatusBadge("Stopped", ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            }

            if (m_app->renderCoordinator().isRendering())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s %s",
                    m_app->renderCoordinator().currentJobId().c_str(),
                    m_app->renderCoordinator().currentChunkLabel().c_str());
            }

            if (!id.systemInfo().gpuName.empty())
                ImGui::Text("GPU: %s", id.systemInfo().gpuName.c_str());
            if (id.systemInfo().cpuCores > 0)
                ImGui::Text("CPU: %d cores  |  RAM: %llu MB",
                    id.systemInfo().cpuCores,
                    static_cast<unsigned long long>(id.systemInfo().ramMB));

            // Node control
            ImGui::Spacing();
            if (m_app->nodeState() == NodeState::Active)
            {
                if (ImGui::Button("Stop Node"))
                    m_app->setNodeState(NodeState::Stopped);
            }
            else
            {
                if (ImGui::Button("Start Node"))
                    m_app->setNodeState(NodeState::Active);
            }

            // --- Peers ---
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("Peers");
            ImGui::Spacing();

            auto peers = m_app->peerManager().getPeerSnapshot();

            if (peers.empty())
            {
                ImGui::TextDisabled("No peers discovered.");
            }
            else
            {
                // Sort: alive first (rendering > idle > stopped), dead last; alpha within
                std::sort(peers.begin(), peers.end(), [](const PeerInfo& a, const PeerInfo& b)
                {
                    if (a.is_alive != b.is_alive)
                        return a.is_alive > b.is_alive;

                    // Rendering > Idle > Stopped
                    auto stateOrder = [](const PeerInfo& p) -> int {
                        if (p.render_state == "rendering") return 0;
                        if (p.node_state == "active") return 1;
                        return 2;
                    };
                    int oa = stateOrder(a), ob = stateOrder(b);
                    if (oa != ob) return oa < ob;

                    return a.hostname < b.hostname;
                });

                for (const auto& peer : peers)
                {
                    ImGui::PushID(peer.node_id.c_str());

                    // Status badge
                    if (!peer.is_alive)
                    {
                        drawStatusBadge("Dead", ImVec4(0.4f, 0.4f, 0.4f, 0.7f));
                    }
                    else if (peer.node_state == "stopped")
                    {
                        drawStatusBadge("Stopped", ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    }
                    else if (peer.render_state == "rendering")
                    {
                        drawStatusBadge("Rendering", ImVec4(0.3f, 0.5f, 1.0f, 1.0f));
                    }
                    else
                    {
                        drawStatusBadge("Idle", ImVec4(0.3f, 0.9f, 0.3f, 1.0f));
                    }

                    ImGui::SameLine();
                    if (!peer.hostname.empty())
                        ImGui::TextUnformatted(peer.hostname.c_str());
                    else
                        ImGui::TextUnformatted(peer.node_id.c_str());

                    // Leader badge
                    if (peer.is_leader)
                    {
                        ImGui::SameLine();
                        drawStatusBadge("Leader", ImVec4(1.0f, 0.84f, 0.0f, 1.0f));
                    }

                    // UDP badge
                    if (peer.has_udp_contact)
                    {
                        ImGui::SameLine();
                        drawStatusBadge("UDP", ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                    }

                    // Suspect badge (suspended by failure tracker)
                    if (m_app->isLeader())
                    {
                        auto& tracker = m_app->dispatchManager().failureTracker();
                        if (tracker.isSuspended(peer.node_id))
                        {
                            ImGui::SameLine();
                            drawStatusBadge("Suspect", ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                            if (ImGui::IsItemHovered())
                            {
                                auto* record = tracker.getRecord(peer.node_id);
                                if (record)
                                {
                                    ImGui::BeginTooltip();
                                    ImGui::Text("%d failures — not receiving new work", record->failure_count);
                                    ImGui::EndTooltip();
                                }
                            }
                        }
                    }

                    // Active job info (if rendering)
                    if (peer.is_alive && peer.render_state == "rendering" && !peer.active_job.empty())
                    {
                        ImGui::TextDisabled("  %s %s", peer.active_job.c_str(), peer.active_chunk.c_str());
                    }

                    // Hardware summary
                    if (peer.is_alive && !peer.hostname.empty())
                    {
                        std::string hw;
                        if (!peer.app_version.empty())
                            hw += "v" + peer.app_version;
                        if (!peer.os.empty())
                        {
                            if (!hw.empty()) hw += " | ";
                            hw += peer.os;
                        }
                        if (peer.cpu_cores > 0)
                        {
                            if (!hw.empty()) hw += " | ";
                            hw += std::to_string(peer.cpu_cores) + " cores";
                        }
                        if (peer.ram_mb > 0)
                        {
                            if (!hw.empty()) hw += " | ";
                            hw += std::to_string(peer.ram_mb / 1024) + " GB";
                        }
                        if (!hw.empty())
                            ImGui::TextDisabled("  %s", hw.c_str());
                    }

                    // Remote control buttons
                    if (peer.is_alive)
                    {
                        ImGui::Indent(16.0f);
                        if (peer.node_state != "stopped")
                        {
                            if (ImGui::SmallButton("Stop"))
                            {
                                m_app->peerManager().setPeerNodeState(peer.node_id, "stopped");
                                std::string ep = peer.endpoint;
                                std::thread([ep]() {
                                    auto [host, port] = parseEndpoint(ep);
                                    if (host.empty()) return;
                                    httplib::Client cli(host, port);
                                    cli.set_connection_timeout(2);
                                    cli.set_read_timeout(2);
                                    cli.Post("/api/node/stop");
                                }).detach();
                            }
                        }
                        else
                        {
                            if (ImGui::SmallButton("Start"))
                            {
                                m_app->peerManager().setPeerNodeState(peer.node_id, "active");
                                std::string ep = peer.endpoint;
                                std::thread([ep]() {
                                    auto [host, port] = parseEndpoint(ep);
                                    if (host.empty()) return;
                                    httplib::Client cli(host, port);
                                    cli.set_connection_timeout(2);
                                    cli.set_read_timeout(2);
                                    cli.Post("/api/node/start");
                                }).detach();
                            }
                        }

                        // Unsuspend button (only leader can unsuspend)
                        if (m_app->isLeader() &&
                            m_app->dispatchManager().failureTracker().isSuspended(peer.node_id))
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Unsuspend"))
                                m_app->unsuspendNode(peer.node_id);
                        }

                        ImGui::Unindent(16.0f);
                    }

                    ImGui::Spacing();
                    ImGui::PopID();
                }
            }
        }
    }
    ImGui::End();
}

} // namespace MR
