#include "monitor/ui/job_detail_panel.h"
#include "monitor/ui/style.h"
#include "monitor/monitor_app.h"
#include "monitor/template_manager.h"
#include "core/platform.h"
#include "core/monitor_log.h"
#include "core/net_utils.h"

#include <imgui.h>
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace MR {

void JobDetailPanel::init(MonitorApp* app)
{
    m_app = app;
}

static ImVec4 stateColor(const std::string& state)
{
    if (state == "active")    return ImVec4(0.3f, 0.8f, 0.3f, 1.0f);
    if (state == "paused")    return ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
    if (state == "completed") return ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
    if (state == "cancelled") return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

static std::string formatTimestampFull(int64_t ms)
{
    if (ms <= 0) return "";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return buf;
}

void JobDetailPanel::render()
{
    if (!visible) return;

    // Check for submission mode request
    if (m_app && m_app->shouldEnterSubmission())
    {
        m_mode = DetailMode::Submission;
        m_selectedTemplateIdx = -1;
        std::memset(m_jobNameBuf, 0, sizeof(m_jobNameBuf));
        std::memset(m_cmdPathBuf, 0, sizeof(m_cmdPathBuf));
        m_flagBufs.clear();
        m_outputBufs.clear();
        m_errors.clear();
    }

    // Auto-switch to detail mode when a job is selected
    if (m_app && !m_app->selectedJobId().empty() && m_mode != DetailMode::Submission)
    {
        m_mode = DetailMode::Detail;
        if (m_detailJobId != m_app->selectedJobId())
        {
            m_detailJobId = m_app->selectedJobId();
            m_hasDetailCache = false;
            m_detailChunksJobId.clear();
        }
    }
    else if (m_app && m_app->selectedJobId().empty() && m_mode == DetailMode::Detail)
    {
        m_mode = DetailMode::Empty;
    }

    if (ImGui::Begin("Job Detail", nullptr, ImGuiWindowFlags_NoTitleBar))
    {
        panelHeader("Job Detail", visible);

        if (!m_app || !m_app->isFarmRunning())
        {
            ImGui::TextDisabled("Farm not connected");
        }
        else
        {
            switch (m_mode)
            {
                case DetailMode::Empty:
                    ImGui::TextDisabled("Select a job from the Job List,");
                    ImGui::TextDisabled("or click New Job to submit one.");
                    break;
                case DetailMode::Submission:
                    renderSubmissionMode();
                    break;
                case DetailMode::Detail:
                    renderDetailMode();
                    break;
            }
        }
    }
    ImGui::End();
}

// --- Submission Mode ---

void JobDetailPanel::onTemplateSelected(int idx)
{
    m_selectedTemplateIdx = idx;
    const auto& templates = m_app->cachedTemplates();
    if (idx < 0 || idx >= (int)templates.size())
        return;

    const auto& tmpl = templates[idx];

    // Fill cmd path for current OS
    std::string os = getOS();
    std::string cmd = getCmdForOS(tmpl.cmd, os);
    std::strncpy(m_cmdPathBuf, cmd.c_str(), sizeof(m_cmdPathBuf) - 1);

    // Allocate flag buffers
    m_flagBufs.resize(tmpl.flags.size());
    m_outputBufs.clear();

    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        m_flagBufs[i] = {};
        const auto& f = tmpl.flags[i];

        if (f.editable && f.value.has_value() && !f.value->empty())
        {
            std::strncpy(m_flagBufs[i].data(), f.value->c_str(), 511);
        }

        if (f.type == "output")
        {
            OutputBuf ob;
            ob.flagIdx = (int)i;
            ob.overridden = false;
            m_outputBufs.push_back(ob);
        }
    }

    // Fill defaults
    m_frameStart = tmpl.job_defaults.frame_start;
    m_frameEnd = tmpl.job_defaults.frame_end;
    m_chunkSize = tmpl.job_defaults.chunk_size;
    m_priority = tmpl.job_defaults.priority;
    m_maxRetries = tmpl.job_defaults.max_retries;
    m_hasTimeout = tmpl.job_defaults.timeout_seconds.has_value();
    m_timeout = m_hasTimeout ? tmpl.job_defaults.timeout_seconds.value() : 0;

    m_errors.clear();
}

void JobDetailPanel::resolveOutputPatterns()
{
    if (m_selectedTemplateIdx < 0)
        return;

    const auto& templates = m_app->cachedTemplates();
    if (m_selectedTemplateIdx >= (int)templates.size())
        return;
    const auto& tmpl = templates[m_selectedTemplateIdx];

    // Collect flag values
    std::vector<std::string> flagValues;
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        if (i < m_flagBufs.size())
            flagValues.push_back(m_flagBufs[i].data());
        else
            flagValues.push_back("");
    }

    for (auto& ob : m_outputBufs)
    {
        if (ob.overridden || ob.flagIdx < 0 || ob.flagIdx >= (int)tmpl.flags.size())
            continue;

        const auto& f = tmpl.flags[ob.flagIdx];
        if (!f.default_pattern.has_value())
            continue;

        std::string resolved = TemplateManager::resolvePattern(
            f.default_pattern.value(), tmpl, flagValues);

        if (!resolved.empty())
        {
            namespace fs = std::filesystem;
            fs::path p(resolved);
            std::string dir = p.parent_path().string();
            std::string fname = p.filename().string();

            std::strncpy(ob.dirBuf.data(), dir.c_str(), ob.dirBuf.size() - 1);
            std::strncpy(ob.filenameBuf.data(), fname.c_str(), ob.filenameBuf.size() - 1);

            // Also update the flagBuf with the full path
            std::strncpy(m_flagBufs[ob.flagIdx].data(), resolved.c_str(),
                m_flagBufs[ob.flagIdx].size() - 1);
        }
    }
}

void JobDetailPanel::renderSubmissionMode()
{
    const auto& templates = m_app->cachedTemplates();

    // Template picker
    ImGui::Text("Template:");
    ImGui::SameLine();
    {
        std::string preview = (m_selectedTemplateIdx >= 0 && m_selectedTemplateIdx < (int)templates.size())
            ? templates[m_selectedTemplateIdx].name
            : "Select template...";

        if (ImGui::BeginCombo("##TemplatePicker", preview.c_str()))
        {
            for (int i = 0; i < (int)templates.size(); ++i)
            {
                const auto& t = templates[i];
                if (!t.valid) continue;

                bool selected = (i == m_selectedTemplateIdx);
                std::string label = t.name;
                if (t.isExample) label += " (example)";

                if (ImGui::Selectable(label.c_str(), selected))
                {
                    onTemplateSelected(i);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (m_selectedTemplateIdx < 0 || m_selectedTemplateIdx >= (int)templates.size())
    {
        ImGui::TextDisabled("Select a template to begin.");
        if (ImGui::Button("Cancel"))
            m_mode = DetailMode::Empty;
        return;
    }

    const auto& tmpl = templates[m_selectedTemplateIdx];
    ImGui::Separator();

    // Job name
    ImGui::Text("Job Name:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##JobName", m_jobNameBuf, sizeof(m_jobNameBuf));

    // Executable path
    if (tmpl.cmd.editable)
    {
        ImGui::Text("%s:", tmpl.cmd.label.empty() ? "Executable" : tmpl.cmd.label.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("##CmdPath", m_cmdPathBuf, sizeof(m_cmdPathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse##Cmd"))
        {
            nfdu8char_t* outPath = nullptr;
            if (NFD_OpenDialogU8(&outPath, nullptr, 0, nullptr) == NFD_OKAY && outPath)
            {
                std::strncpy(m_cmdPathBuf, outPath, sizeof(m_cmdPathBuf) - 1);
                NFD_FreePathU8(outPath);
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Flags:");

    // Track if any file flag changed (for output pattern re-resolve)
    bool needResolve = false;

    // Editable flags
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        const auto& f = tmpl.flags[i];
        if (!f.editable) continue;
        if (i >= m_flagBufs.size()) continue;

        // Output flags rendered separately
        if (f.type == "output") continue;

        std::string label = f.info.empty() ? f.flag : f.info;
        if (f.required) label += " *";

        ImGui::Text("%s:", label.c_str());
        ImGui::SameLine();

        std::string id = "##Flag" + std::to_string(i);

        if (f.type == "file")
        {
            ImGui::SetNextItemWidth(-60.0f);
            if (ImGui::InputText(id.c_str(), m_flagBufs[i].data(), m_flagBufs[i].size()))
                needResolve = true;
            ImGui::SameLine();

            std::string browseId = "Browse##" + std::to_string(i);
            if (ImGui::Button(browseId.c_str()))
            {
                nfdu8filteritem_t filter = {};
                nfdu8filteritem_t* filterPtr = nullptr;
                int filterCount = 0;

                std::string filterName, filterSpec;
                if (!f.filter.empty())
                {
                    filterName = f.filter + " files";
                    filterSpec = f.filter;
                    filter.name = filterName.c_str();
                    filter.spec = filterSpec.c_str();
                    filterPtr = &filter;
                    filterCount = 1;
                }

                nfdu8char_t* outPath = nullptr;
                if (NFD_OpenDialogU8(&outPath, filterPtr, filterCount, nullptr) == NFD_OKAY && outPath)
                {
                    std::strncpy(m_flagBufs[i].data(), outPath, m_flagBufs[i].size() - 1);
                    NFD_FreePathU8(outPath);
                    needResolve = true;
                }
            }
        }
        else
        {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText(id.c_str(), m_flagBufs[i].data(), m_flagBufs[i].size());
        }
    }

    // Output flags (split dir/filename)
    for (auto& ob : m_outputBufs)
    {
        if (ob.flagIdx < 0 || ob.flagIdx >= (int)tmpl.flags.size()) continue;
        const auto& f = tmpl.flags[ob.flagIdx];

        std::string label = f.info.empty() ? "Output" : f.info;
        if (f.required) label += " *";
        ImGui::Text("%s:", label.c_str());

        // Directory
        {
            std::string id = "##OutDir" + std::to_string(ob.flagIdx);
            ImGui::Text("  Dir:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-60.0f);
            if (ImGui::InputText(id.c_str(), ob.dirBuf.data(), ob.dirBuf.size()))
                ob.overridden = true;
            ImGui::SameLine();

            std::string browseId = "Browse##OutDir" + std::to_string(ob.flagIdx);
            if (ImGui::Button(browseId.c_str()))
            {
                nfdu8char_t* outPath = nullptr;
                if (NFD_PickFolderU8(&outPath, nullptr) == NFD_OKAY && outPath)
                {
                    std::strncpy(ob.dirBuf.data(), outPath, ob.dirBuf.size() - 1);
                    NFD_FreePathU8(outPath);
                    ob.overridden = true;
                }
            }
        }

        // Filename
        {
            std::string id = "##OutFile" + std::to_string(ob.flagIdx);
            ImGui::Text("  File:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText(id.c_str(), ob.filenameBuf.data(), ob.filenameBuf.size()))
                ob.overridden = true;
        }

        // Sync back to flagBuf
        {
            std::string dir(ob.dirBuf.data());
            std::string file(ob.filenameBuf.data());
            std::string full;
            if (!dir.empty() && !file.empty())
            {
                full = (std::filesystem::path(dir) / file).string();
            }
            else if (!dir.empty())
            {
                full = dir;
            }
            else
            {
                full = file;
            }
            std::strncpy(m_flagBufs[ob.flagIdx].data(), full.c_str(),
                m_flagBufs[ob.flagIdx].size() - 1);
        }
    }

    // Re-resolve output patterns if file inputs changed
    if (needResolve)
        resolveOutputPatterns();

    ImGui::Separator();

    // Frame range / chunk / priority / retries / timeout
    ImGui::Text("Frame Range:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##FrameStart", &m_frameStart, 0);
    ImGui::SameLine();
    ImGui::Text("-");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##FrameEnd", &m_frameEnd, 0);

    ImGui::Text("Chunk Size:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##ChunkSize", &m_chunkSize, 0);
    if (m_chunkSize < 1) m_chunkSize = 1;

    ImGui::Text("Priority:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##Priority", &m_priority, 0);

    ImGui::Text("Max Retries:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("##MaxRetries", &m_maxRetries, 0);
    if (m_maxRetries < 0) m_maxRetries = 0;

    ImGui::Checkbox("Timeout", &m_hasTimeout);
    if (m_hasTimeout)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("##Timeout", &m_timeout, 0);
        ImGui::SameLine();
        ImGui::Text("seconds");
    }

    // Command preview
    ImGui::Separator();
    ImGui::Text("Command Preview:");
    {
        std::vector<std::string> flagVals;
        for (size_t i = 0; i < tmpl.flags.size(); ++i)
        {
            if (i < m_flagBufs.size())
                flagVals.push_back(m_flagBufs[i].data());
            else
                flagVals.push_back("");
        }
        // buildCommandPreview is a const method; use a temp TemplateManager
        TemplateManager previewTm;
        std::string preview = previewTm.buildCommandPreview(tmpl, flagVals, m_cmdPathBuf);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::TextWrapped("%s", preview.c_str());
        ImGui::PopStyleColor();
    }

    // Validation errors
    if (!m_errors.empty())
    {
        ImGui::Separator();
        for (const auto& err : m_errors)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();

    // Handle async submit result
    if (m_asyncSubmitting)
    {
        std::lock_guard<std::mutex> lock(m_asyncResultMutex);
        if (m_asyncResult != 0)
        {
            m_asyncSubmitting = false;
            if (m_asyncResult == 1)
            {
                MonitorLog::instance().info("job", "Submitted job: " + m_asyncSubmitSlug);
                m_app->selectJob(m_asyncSubmitSlug);
                m_mode = DetailMode::Detail;
                m_detailJobId = m_asyncSubmitSlug;
                m_hasDetailCache = false;
                m_detailChunksJobId.clear();
                m_asyncSubmitSlug.clear();
                m_asyncResult = 0;
            }
            else
            {
                m_errors.push_back(m_asyncResultError);
                m_asyncResultError.clear();
                m_asyncResult = 0;
                m_asyncSubmitSlug.clear();
            }
        }
    }

    // Submit / Cancel buttons
    if (m_asyncSubmitting)
    {
        ImGui::TextDisabled("Submitting...");
    }
    else if (ImGui::Button("Submit"))
    {
        doSubmit();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        m_mode = DetailMode::Empty;
        m_app->selectJob("");
    }
}

void JobDetailPanel::doSubmit()
{
    const auto& templates = m_app->cachedTemplates();
    if (m_selectedTemplateIdx < 0 || m_selectedTemplateIdx >= (int)templates.size())
        return;

    const auto& tmpl = templates[m_selectedTemplateIdx];

    // Collect flag values
    std::vector<std::string> flagValues;
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        if (i < m_flagBufs.size())
            flagValues.push_back(m_flagBufs[i].data());
        else
            flagValues.push_back("");
    }

    // Validate
    m_errors = TemplateManager::validateSubmission(
        tmpl, flagValues, m_cmdPathBuf, m_jobNameBuf,
        m_frameStart, m_frameEnd, m_chunkSize,
        m_app->farmPath() / "jobs");

    if (!m_errors.empty())
        return;

    // Generate slug
    std::string slug = TemplateManager::generateSlug(
        m_jobNameBuf, m_app->farmPath() / "jobs");

    if (slug.empty())
    {
        m_errors.push_back("Failed to generate job slug");
        return;
    }

    // Bake manifest
    std::optional<int> timeout;
    if (m_hasTimeout && m_timeout > 0)
        timeout = m_timeout;

    auto manifest = TemplateManager::bakeManifestStatic(
        tmpl, flagValues, m_cmdPathBuf, slug,
        m_frameStart, m_frameEnd, m_chunkSize,
        m_maxRetries, timeout,
        m_app->identity().nodeId(), getOS());

    // Submit to leader (manifest stored in SQLite, no shared FS backup needed)
    if (m_app->isLeader())
    {
        m_app->dispatchManager().submitJob(manifest, m_priority);
        MonitorLog::instance().info("job", "Submitted job: " + slug);

        // Switch to detail mode for the new job
        m_app->selectJob(slug);
        m_mode = DetailMode::Detail;
        m_detailJobId = slug;
        m_hasDetailCache = false;
        m_detailChunksJobId.clear();
    }
    else
    {
        nlohmann::json body = {
            {"manifest", nlohmann::json(manifest)},
            {"priority", m_priority},
        };

        m_asyncSubmitting = true;
        m_asyncSubmitSlug = slug;
        {
            std::lock_guard<std::mutex> lock(m_asyncResultMutex);
            m_asyncResult = 0;
            m_asyncResultError.clear();
        }

        m_app->postToLeaderAsync("/api/jobs", body.dump(),
            [this](bool success, const std::string&) {
                std::lock_guard<std::mutex> lock(m_asyncResultMutex);
                m_asyncResult = success ? 1 : -1;
                if (!success)
                    m_asyncResultError = "Failed to submit job to leader";
            });
    }
}

// --- Detail Mode ---

void JobDetailPanel::renderDetailMode()
{
    const auto& jobs = m_app->cachedJobs();

    // Find the job in cached data
    const JobInfo* jobPtr = nullptr;
    for (const auto& j : jobs)
    {
        if (j.manifest.job_id == m_detailJobId)
        {
            jobPtr = &j;
            m_cachedDetail = j;
            m_hasDetailCache = true;
            break;
        }
    }

    // Use cache for flicker prevention
    if (!jobPtr && m_hasDetailCache)
        jobPtr = &m_cachedDetail;

    if (!jobPtr)
    {
        ImGui::TextDisabled("Job not found: %s", m_detailJobId.c_str());
        return;
    }

    const auto& job = *jobPtr;

    // Header: job_id + state badge
    ImGui::PushStyleColor(ImGuiCol_Text, stateColor(job.current_state));
    ImGui::Text("[%s]", job.current_state.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (Fonts::bold) ImGui::PushFont(Fonts::bold);
    ImGui::Text("%s", job.manifest.job_id.c_str());
    if (Fonts::bold) ImGui::PopFont();

    // Info line
    ImGui::Text("Template: %s  |  Priority: %d  |  By: %s",
        job.manifest.template_id.c_str(),
        job.current_priority,
        job.manifest.submitted_by.c_str());
    ImGui::Text("Submitted: %s  |  Frames: %d-%d (chunk %d)",
        formatTimestampFull(job.manifest.submitted_at_ms).c_str(),
        job.manifest.frame_start, job.manifest.frame_end, job.manifest.chunk_size);

    ImGui::Separator();

    // Progress bar
    if (job.total_chunks > 0)
    {
        float fraction = static_cast<float>(job.completed_chunks) / static_cast<float>(job.total_chunks);
        char overlay[128];
        snprintf(overlay, sizeof(overlay), "%d/%d completed, %d rendering, %d failed",
            job.completed_chunks, job.total_chunks, job.rendering_chunks, job.failed_chunks);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::PopStyleColor();
    }

    // Frame grid (leader or worker — uses MonitorApp::getChunksForJob dual-path)
    if (m_app->isFarmRunning())
    {
        bool needsRefresh = (m_detailChunksJobId != m_detailJobId);
        if (!needsRefresh && job.current_state == "active")
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - m_lastChunkRefresh).count();
            if (elapsed > 3000)
                needsRefresh = true;
        }
        // Force one final refresh when job transitions to a terminal state
        if (!needsRefresh && job.current_state != m_detailChunksLastState
            && (job.current_state == "completed" || job.current_state == "cancelled"
                || job.current_state == "failed"))
        {
            needsRefresh = true;
        }
        if (needsRefresh)
        {
            m_detailChunks = m_app->getChunksForJob(m_detailJobId);
            m_detailChunksJobId = m_detailJobId;
            m_detailChunksLastState = job.current_state;
            m_lastChunkRefresh = std::chrono::steady_clock::now();
        }

        if (!m_detailChunks.empty())
        {
            ImGui::Separator();
            ImGui::Text("Frames:");
            renderFrameGrid(m_detailChunks, job.manifest.frame_start, job.manifest.frame_end);
        }
    }

    ImGui::Separator();

    // Control buttons (state-dependent)
    if (job.current_state == "active")
    {
        if (ImGui::Button("Pause"))
            m_app->pauseJob(m_detailJobId);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            m_pendingCancel = true;
    }
    else if (job.current_state == "paused")
    {
        if (ImGui::Button("Resume"))
            m_app->resumeJob(m_detailJobId);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            m_pendingCancel = true;
    }
    else if (job.current_state == "completed" || job.current_state == "cancelled"
             || job.current_state == "failed")
    {
        if (ImGui::Button("Resubmit"))
            m_pendingResubmit = true;
        ImGui::SameLine();
        if (ImGui::Button("Archive"))
            m_app->archiveJob(m_detailJobId);
        ImGui::SameLine();
    }

    // Retry Failed — shown when any chunks have failed
    if (job.failed_chunks > 0)
    {
        if (ImGui::Button("Retry Failed"))
            m_pendingRetryFailed = true;
        ImGui::SameLine();
    }

    if (ImGui::Button("Delete"))
        m_pendingDelete = true;

    // Open Output folder button
    if (job.manifest.output_dir.has_value() && !job.manifest.output_dir.value().empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("Open Output"))
            openFolderInExplorer(std::filesystem::path(job.manifest.output_dir.value()));
    }

    // Confirmation popups
    if (m_pendingCancel)
    {
        ImGui::OpenPopup("Confirm Cancel");
        m_pendingCancel = false;
    }
    if (ImGui::BeginPopupModal("Confirm Cancel", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Cancel job '%s'? Active renders will be aborted.", m_detailJobId.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Yes, Cancel"))
        {
            m_app->cancelJob(m_detailJobId);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_pendingResubmit)
    {
        ImGui::OpenPopup("Confirm Resubmit");
        m_pendingResubmit = false;
    }
    if (ImGui::BeginPopupModal("Confirm Resubmit", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Create a new job from this manifest?");
        ImGui::Text("The original job will be preserved.");
        ImGui::Spacing();
        if (ImGui::Button("Resubmit"))
        {
            m_app->resubmitJob(m_detailJobId);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_pendingRetryFailed)
    {
        ImGui::OpenPopup("Confirm Retry Failed");
        m_pendingRetryFailed = false;
    }
    if (ImGui::BeginPopupModal("Confirm Retry Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Re-render %d failed chunks?", job.failed_chunks);
        ImGui::Text("Completed frames will be preserved.");
        ImGui::Spacing();
        if (ImGui::Button("Retry Failed"))
        {
            m_app->retryFailedChunks(m_detailJobId);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_pendingDelete)
    {
        ImGui::OpenPopup("Confirm Delete");
        m_pendingDelete = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Delete job '%s'? This cannot be undone.", m_detailJobId.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Yes, Delete"))
        {
            m_app->deleteJob(m_detailJobId);
            m_mode = DetailMode::Empty;
            m_detailJobId.clear();
            m_hasDetailCache = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void JobDetailPanel::renderFrameGrid(const std::vector<ChunkRow>& chunks, int fStart, int fEnd)
{
    if (chunks.empty() || fEnd < fStart) return;

    // Expand chunks to per-frame states
    int totalFrames = fEnd - fStart + 1;
    struct FrameVis {
        std::string state = "pending";
        std::string assigned_to;
        int retry_count = 0;
        std::vector<std::string> failed_on;
    };
    std::vector<FrameVis> frames(totalFrames);

    for (const auto& c : chunks)
    {
        for (int f = c.frame_start; f <= c.frame_end; ++f)
        {
            int idx = f - fStart;
            if (idx >= 0 && idx < totalFrames)
            {
                frames[idx].state = c.state;
                frames[idx].assigned_to = c.assigned_to;
                frames[idx].retry_count = c.retry_count;
                frames[idx].failed_on = c.failed_on;
            }
        }
    }

    // Upgrade per-frame states from completed_frames within "assigned" chunks
    for (const auto& c : chunks)
    {
        if (c.state == "assigned" && !c.completed_frames.empty())
        {
            for (int f : c.completed_frames)
            {
                int idx = f - fStart;
                if (idx >= 0 && idx < totalFrames)
                    frames[idx].state = "completed";
            }
        }
    }

    // Draw per-frame grid
    const float cellSize = 14.0f;
    const float gap = 2.0f;
    float availWidth = ImGui::GetContentRegionAvail().x;
    int cols = (std::max)(1, (int)(availWidth / (cellSize + gap)));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Pass 1: colored rectangles
    for (int i = 0; i < totalFrames; ++i)
    {
        int col = i % cols;
        int row = i / cols;
        float x = origin.x + col * (cellSize + gap);
        float y = origin.y + row * (cellSize + gap);

        ImU32 color;
        const auto& st = frames[i].state;
        if (st == "assigned")       color = IM_COL32(60, 140, 220, 255);
        else if (st == "completed") color = IM_COL32(60, 180, 60, 255);
        else if (st == "failed")    color = IM_COL32(200, 50, 50, 255);
        else                        color = IM_COL32(64, 64, 64, 255);

        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cellSize, y + cellSize), color, 2.0f);
    }

    // Pass 2: hover tooltips via InvisibleButton
    ImGui::PushID("##framegrid");
    for (int i = 0; i < totalFrames; ++i)
    {
        int col = i % cols;
        int row = i / cols;
        float x = origin.x + col * (cellSize + gap);
        float y = origin.y + row * (cellSize + gap);

        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##cell", ImVec2(cellSize, cellSize));

        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Frame %d", fStart + i);
            ImGui::Text("State: %s", frames[i].state.c_str());
            if (!frames[i].assigned_to.empty())
                ImGui::Text("Assigned: %s", frames[i].assigned_to.c_str());
            if (frames[i].retry_count > 0)
                ImGui::Text("Retries: %d", frames[i].retry_count);
            if (!frames[i].failed_on.empty())
            {
                std::string nodes;
                for (const auto& n : frames[i].failed_on)
                {
                    if (!nodes.empty()) nodes += ", ";
                    nodes += n;
                }
                ImGui::Text("Failed on: %s", nodes.c_str());
            }
            ImGui::EndTooltip();
        }

        ImGui::PopID();
    }
    ImGui::PopID();

    int totalRows = (totalFrames + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + totalRows * (cellSize + gap) + 4.0f));
}

} // namespace MR
