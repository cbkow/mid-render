#include "monitor/submission_watcher.h"
#include "monitor/monitor_app.h"
#include "monitor/template_manager.h"
#include "core/platform.h"
#include "core/monitor_log.h"
#include "core/net_utils.h"

#include <nlohmann/json.hpp>
#include <httplib.h>
#include <fstream>

namespace MR {

namespace fs = std::filesystem;

void SubmissionWatcher::init(MonitorApp* app, const fs::path& appDataDir)
{
    m_app = app;
    m_submissionsDir = appDataDir / "submissions";

    std::error_code ec;
    fs::create_directories(m_submissionsDir / "processed", ec);
}

void SubmissionWatcher::poll()
{
    if (!m_app || !m_app->isFarmRunning())
        return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastPoll).count();
    if (elapsed < 3000)
        return;
    m_lastPoll = now;

    std::error_code ec;
    if (!fs::is_directory(m_submissionsDir, ec))
        return;

    for (auto& entry : fs::directory_iterator(m_submissionsDir, ec))
    {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;

        processSubmission(entry.path());
    }
}

void SubmissionWatcher::processSubmission(const fs::path& jsonPath)
{
    nlohmann::json sub;
    try
    {
        std::ifstream ifs(jsonPath);
        sub = nlohmann::json::parse(ifs);
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("job",
            "DCC submit: failed to parse " + jsonPath.filename().string() + ": " + e.what());
        // Move to processed so we don't retry broken files
        std::error_code ec;
        fs::rename(jsonPath, m_submissionsDir / "processed" / jsonPath.filename(), ec);
        return;
    }

    std::string templateId = sub.value("template_id", "");
    std::string jobName = sub.value("job_name", "");
    int frameStart = sub.value("frame_start", 1);
    int frameEnd = sub.value("frame_end", 250);
    int chunkSize = sub.value("chunk_size", 10);
    int priority = sub.value("priority", 50);
    auto overrides = sub.value("overrides", nlohmann::json::object());

    if (templateId.empty() || jobName.empty())
    {
        MonitorLog::instance().error("job", "DCC submit: missing template_id or job_name");
        std::error_code ec;
        fs::rename(jsonPath, m_submissionsDir / "processed" / jsonPath.filename(), ec);
        return;
    }

    // Find template — first check cached (monitor) templates, then plugins dir
    const auto& templates = m_app->cachedTemplates();
    const JobTemplate* tmplPtr = nullptr;
    for (const auto& t : templates)
    {
        if (t.template_id == templateId && t.valid)
        {
            tmplPtr = &t;
            break;
        }
    }

    // Fallback: scan templates/plugins/ for DCC-specific templates
    JobTemplate pluginTmpl;
    if (!tmplPtr)
    {
        auto pluginsDir = m_app->farmPath() / "templates" / "plugins";
        std::error_code pec;
        if (fs::is_directory(pluginsDir, pec))
        {
            for (auto& entry : fs::directory_iterator(pluginsDir, pec))
            {
                if (!entry.is_regular_file(pec) || entry.path().extension() != ".json")
                    continue;
                try
                {
                    std::ifstream tifs(entry.path());
                    auto tj = nlohmann::json::parse(tifs);
                    auto candidate = tj.get<JobTemplate>();
                    if (candidate.template_id == templateId)
                    {
                        TemplateManager::validateTemplate(candidate);
                        if (candidate.valid)
                        {
                            pluginTmpl = std::move(candidate);
                            tmplPtr = &pluginTmpl;
                            break;
                        }
                    }
                }
                catch (...) {}
            }
        }
    }

    if (!tmplPtr)
    {
        MonitorLog::instance().error("job",
            "DCC submit: template not found: " + templateId);
        std::error_code ec;
        fs::rename(jsonPath, m_submissionsDir / "processed" / jsonPath.filename(), ec);
        return;
    }

    const auto& tmpl = *tmplPtr;

    // Build flagValues: start with template defaults, apply overrides by id
    std::vector<std::string> flagValues;
    for (size_t i = 0; i < tmpl.flags.size(); ++i)
    {
        const auto& f = tmpl.flags[i];

        // Check if override exists for this flag's id
        if (!f.id.empty() && overrides.contains(f.id) && overrides[f.id].is_string())
        {
            flagValues.push_back(overrides[f.id].get<std::string>());
        }
        else if (f.value.has_value())
        {
            flagValues.push_back(f.value.value());
        }
        else
        {
            flagValues.push_back("");
        }
    }

    // Get cmd for current OS
    std::string os = getOS();
    std::string cmd = getCmdForOS(tmpl.cmd, os);

    // Generate slug
    std::string slug = TemplateManager::generateSlug(jobName, m_app->farmPath() / "jobs");
    if (slug.empty())
    {
        MonitorLog::instance().error("job", "DCC submit: failed to generate slug for: " + jobName);
        std::error_code ec;
        fs::rename(jsonPath, m_submissionsDir / "processed" / jsonPath.filename(), ec);
        return;
    }

    // Bake manifest
    auto manifest = TemplateManager::bakeManifestStatic(
        tmpl, flagValues, cmd, slug,
        frameStart, frameEnd, chunkSize,
        tmpl.job_defaults.max_retries,
        tmpl.job_defaults.timeout_seconds,
        m_app->identity().nodeId(), os);

    // Submit to leader (job stored in SQLite, no shared FS write needed)
    if (m_app->isLeader())
    {
        m_app->dispatchManager().submitJob(manifest, priority);
    }
    else
    {
        std::string ep = m_app->getLeaderEndpoint();
        if (!ep.empty())
        {
            auto [host, port] = parseEndpoint(ep);
            if (!host.empty())
            {
                try
                {
                    httplib::Client cli(host, port);
                    cli.set_connection_timeout(3);
                    cli.set_read_timeout(5);

                    nlohmann::json body = {
                        {"manifest", nlohmann::json(manifest)},
                        {"priority", priority},
                    };
                    auto res = cli.Post("/api/jobs", body.dump(), "application/json");
                    if (!res || res->status != 200)
                    {
                        MonitorLog::instance().warn("job",
                            "DCC submit: failed to send to leader for " + slug);
                    }
                }
                catch (const std::exception& e)
                {
                    MonitorLog::instance().warn("job",
                        "DCC submit: leader error for " + slug + ": " + e.what());
                }
            }
        }
        else
        {
            MonitorLog::instance().warn("job", "DCC submit: no leader available for " + slug);
        }
    }

    MonitorLog::instance().info("job", "DCC submit: " + slug);

    // Move to processed/
    std::error_code ec;
    fs::rename(jsonPath, m_submissionsDir / "processed" / jsonPath.filename(), ec);
}

} // namespace MR
