#pragma once

#include "core/ipc_server.h"

#include <nlohmann/json.hpp>

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace MR {

/// Manages the agent process lifecycle and IPC communication.
class AgentSupervisor
{
public:
    AgentSupervisor();
    ~AgentSupervisor();

    AgentSupervisor(const AgentSupervisor&) = delete;
    AgentSupervisor& operator=(const AgentSupervisor&) = delete;

    void start(const std::string& nodeId);
    void stop();

    bool spawnAgent();
    void shutdownAgent();
    void killAgent();
    void sendPing();
    bool sendTask(const std::string& taskJson);
    void sendAbort(const std::string& reason);

    void setMessageHandler(std::function<void(const std::string&, const nlohmann::json&)> handler);
    void processMessages();

    bool isAgentRunning() const;
    bool isAgentConnected() const { return m_ipc.isConnected(); }
    uint32_t agentPid() const { return m_agentPid; }
    const std::string& agentState() const { return m_agentState; }

private:
    void ipcThreadFunc();
    bool sendJson(const std::string& json);

    IpcServer m_ipc;
    std::string m_nodeId;

    std::thread m_ipcThread;
    std::atomic<bool> m_running{false};

    std::queue<std::string> m_messageQueue;
    std::mutex m_queueMutex;

#ifdef _WIN32
    HANDLE m_processHandle = nullptr;
    HANDLE m_threadHandle = nullptr;
#endif
    uint32_t m_agentPid = 0;
    std::string m_agentState;

    std::chrono::steady_clock::time_point m_lastPingTime;
    static constexpr int PING_INTERVAL_SECONDS = 30;

    std::function<void(const std::string&, const nlohmann::json&)> m_messageHandler;
};

} // namespace MR
