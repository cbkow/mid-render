#pragma once

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace MR {

class SingleInstance
{
public:
    explicit SingleInstance(const std::string& name);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Returns true if this is the first instance (no existing monitor running)
    bool isFirst() const;

    // Signal existing instance to show its window (via tray HWND message)
    void signalExisting();

private:
#ifdef _WIN32
    HANDLE m_mutex = nullptr;
    bool m_isFirst = false;
#endif
};

} // namespace MR
