#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <chrono>
#include <utility>

namespace MR {

class NodeFailureTracker
{
public:
    struct Record
    {
        int failure_count = 0;
        int64_t first_failure_ms = 0;
        int64_t last_failure_ms = 0;
        bool suspended = false;
    };

    void recordFailure(const std::string& nodeId, int64_t nowMs)
    {
        auto& r = m_records[nodeId];

        // If the first failure is outside the window, reset the counter
        if (r.first_failure_ms > 0 && (nowMs - r.first_failure_ms) > SUSPEND_WINDOW_MS)
        {
            r.failure_count = 0;
            r.first_failure_ms = nowMs;
        }

        if (r.failure_count == 0)
            r.first_failure_ms = nowMs;

        r.failure_count++;
        r.last_failure_ms = nowMs;

        if (r.failure_count >= SUSPEND_THRESHOLD)
            r.suspended = true;
    }

    bool isSuspended(const std::string& nodeId) const
    {
        auto it = m_records.find(nodeId);
        if (it == m_records.end())
            return false;
        return it->second.suspended;
    }

    void clearNode(const std::string& nodeId)
    {
        m_records.erase(nodeId);
    }

    void clearAll()
    {
        m_records.clear();
    }

    std::vector<std::pair<std::string, Record>> getSuspended() const
    {
        std::vector<std::pair<std::string, Record>> result;
        for (const auto& [id, r] : m_records)
        {
            if (r.suspended)
                result.emplace_back(id, r);
        }
        return result;
    }

    const Record* getRecord(const std::string& nodeId) const
    {
        auto it = m_records.find(nodeId);
        if (it == m_records.end())
            return nullptr;
        return &it->second;
    }

private:
    std::unordered_map<std::string, Record> m_records;

    static constexpr int SUSPEND_THRESHOLD = 5;
    static constexpr int64_t SUSPEND_WINDOW_MS = 300000; // 5 minutes
};

} // namespace MR
