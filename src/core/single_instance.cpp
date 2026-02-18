#include "core/single_instance.h"
#include "core/platform.h"

#include <iostream>

namespace MR {

#ifdef _WIN32

SingleInstance::SingleInstance(const std::string& name)
{
    // Create a named mutex. If it already exists, GetLastError() == ERROR_ALREADY_EXISTS.
    std::wstring wideName(name.begin(), name.end());
    m_mutex = CreateMutexW(nullptr, FALSE, wideName.c_str());
    m_isFirst = (GetLastError() != ERROR_ALREADY_EXISTS);
}

SingleInstance::~SingleInstance()
{
    if (m_mutex)
    {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
        m_mutex = nullptr;
    }
}

bool SingleInstance::isFirst() const
{
    return m_isFirst;
}

void SingleInstance::signalExisting()
{
    // Find the tray's message-only HWND and post a custom message
    HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, L"MidRenderTray", nullptr);
    if (hwnd)
    {
        // WM_APP + 2 = "show window" signal
        PostMessageW(hwnd, WM_APP + 2, 0, 0);
    }
}

#else

// Stub for non-Windows
SingleInstance::SingleInstance(const std::string& /*name*/) {}
SingleInstance::~SingleInstance() {}
bool SingleInstance::isFirst() const { return true; }
void SingleInstance::signalExisting() {}

#endif

} // namespace MR
