#include "core/http_server.h"
#include "core/peer_info.h"
#include "core/monitor_log.h"
#include "monitor/monitor_app.h"
#include "monitor/dispatch_manager.h"
#include "monitor/database_manager.h"

#include <nlohmann/json.hpp>

namespace MR {

void HttpServer::init(MonitorApp* app)
{
    m_app = app;
}

bool HttpServer::requireLeader(httplib::Response& res)
{
    if (!m_app->isLeader())
    {
        nlohmann::json body = {{"error", "not_leader"}};
        auto peers = m_app->peerManager().getPeerSnapshot();
        for (const auto& p : peers)
        {
            if (p.is_leader)
            {
                body["leader_endpoint"] = p.endpoint;
                break;
            }
        }
        res.status = 503;
        res.set_content(body.dump(), "application/json");
        return false;
    }
    return true;
}

void HttpServer::setupRoutes()
{
    // GET /api/status -- this node's PeerInfo as JSON
    m_server.Get("/api/status", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }

        PeerInfo info = m_app->buildLocalPeerInfo();
        nlohmann::json j = info;
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/peers -- list of known peers (from PeerManager snapshot)
    m_server.Get("/api/peers", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }

        auto peers = m_app->peerManager().getPeerSnapshot();
        nlohmann::json j = peers;
        res.set_content(j.dump(), "application/json");
    });

    // --- Remote node control (every node) ---

    // POST /api/node/stop -- remotely stop rendering on this node
    m_server.Post("/api/node/stop", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        m_app->setNodeState(NodeState::Stopped);
        MonitorLog::instance().info("farm", "Remotely stopped by peer");
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/node/start -- remotely resume rendering on this node
    m_server.Post("/api/node/start", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        m_app->setNodeState(NodeState::Active);
        MonitorLog::instance().info("farm", "Remotely started by peer");
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // --- Worker endpoint (every node) ---

    // POST /api/dispatch/assign -- receives assignment from leader
    m_server.Post("/api/dispatch/assign", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }

        // If this node is currently rendering, reject
        if (m_app->renderCoordinator().isRendering())
        {
            res.status = 409;
            res.set_content(R"({"error":"busy"})", "application/json");
            return;
        }

        // If stopped, reject
        if (m_app->nodeState() == NodeState::Stopped)
        {
            res.status = 409;
            res.set_content(R"({"error":"stopped"})", "application/json");
            return;
        }

        try
        {
            auto body = nlohmann::json::parse(req.body);
            auto manifest = body.at("manifest").get<JobManifest>();
            int frameStart = body.at("frame_start").get<int>();
            int frameEnd = body.at("frame_end").get<int>();

            ChunkRange chunk{frameStart, frameEnd};
            m_app->renderCoordinator().queueDispatch(manifest, chunk);

            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // --- Leader-only endpoints ---

    // POST /api/jobs -- submit a new job
    m_server.Post("/api/jobs", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            auto manifest = body.at("manifest").get<JobManifest>();
            int priority = body.value("priority", 50);

            SubmitRequest sr;
            sr.manifest = std::move(manifest);
            sr.priority = priority;
            m_app->dispatchManager().queueSubmission(std::move(sr));

            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // GET /api/jobs -- list all jobs with progress
    m_server.Get("/api/jobs", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        auto json = m_app->getCachedJobsJson();
        res.set_content(json, "application/json");
    });

    // GET /api/jobs/:id -- single job detail + chunks
    m_server.Get(R"(/api/jobs/([^/]+))", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        std::string jobId = req.matches[1];
        auto json = m_app->getCachedJobDetailJson(jobId);
        if (json.empty())
        {
            res.status = 404;
            res.set_content(R"({"error":"not_found"})", "application/json");
            return;
        }
        res.set_content(json, "application/json");
    });

    // POST /api/jobs/:id/pause
    m_server.Post(R"(/api/jobs/([^/]+)/pause)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->pauseJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/resume
    m_server.Post(R"(/api/jobs/([^/]+)/resume)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->resumeJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/cancel
    m_server.Post(R"(/api/jobs/([^/]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->cancelJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/archive
    m_server.Post(R"(/api/jobs/([^/]+)/archive)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->archiveJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // DELETE /api/jobs/:id
    m_server.Delete(R"(/api/jobs/([^/]+))", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->deleteJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/dispatch/frame-complete -- worker reports per-frame completions
    m_server.Post("/api/dispatch/frame-complete", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            std::string nodeId = body.value("node_id", "");
            std::string jobId = body.at("job_id").get<std::string>();
            auto frames = body.at("frames").get<std::vector<int>>();

            for (int frame : frames)
            {
                FrameReport fr;
                fr.node_id = nodeId;
                fr.job_id = jobId;
                fr.frame = frame;
                m_app->dispatchManager().queueFrameCompletion(std::move(fr));
            }

            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/dispatch/complete -- worker reports chunk completion
    m_server.Post("/api/dispatch/complete", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            CompletionReport report;
            report.node_id = body.value("node_id", "");
            report.job_id = body.at("job_id").get<std::string>();
            report.frame_start = body.at("frame_start").get<int>();
            report.frame_end = body.at("frame_end").get<int>();
            report.elapsed_ms = body.value("elapsed_ms", int64_t(0));
            report.exit_code = body.value("exit_code", 0);

            m_app->dispatchManager().queueCompletion(std::move(report));
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/dispatch/failed -- worker reports chunk failure
    m_server.Post("/api/dispatch/failed", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            FailureReport report;
            report.node_id = body.value("node_id", "");
            report.job_id = body.at("job_id").get<std::string>();
            report.frame_start = body.at("frame_start").get<int>();
            report.frame_end = body.at("frame_end").get<int>();
            report.error = body.value("error", std::string("Unknown"));

            m_app->dispatchManager().queueFailure(std::move(report));
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });
}

bool HttpServer::start(const std::string& bindAddress, uint16_t port)
{
    if (m_running.load())
        return true;

    setupRoutes();
    m_port = port;

    // Try to bind before launching thread
    if (!m_server.bind_to_port(bindAddress, port))
    {
        MonitorLog::instance().error("http", "Failed to bind HTTP server to " +
            bindAddress + ":" + std::to_string(port));
        return false;
    }

    m_running.store(true);
    m_thread = std::thread([this]()
    {
        MonitorLog::instance().info("http", "HTTP server listening on port " +
            std::to_string(m_port));
        m_server.listen_after_bind();
        m_running.store(false);
    });

    return true;
}

void HttpServer::stop()
{
    if (!m_running.load())
        return;

    m_server.stop();
    if (m_thread.joinable())
        m_thread.join();

    m_running.store(false);
    MonitorLog::instance().info("http", "HTTP server stopped");
}

} // namespace MR
