#include "core/platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#endif

#include <iostream>

namespace MR {

std::filesystem::path getAppDataDir()
{
#ifdef _WIN32
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)))
    {
        std::filesystem::path dir = std::filesystem::path(appData) / L"MidRender";
        CoTaskMemFree(appData);
        ensureDir(dir);
        return dir;
    }
    CoTaskMemFree(appData);
#endif
    // Fallback
    auto dir = std::filesystem::current_path() / "MidRender_data";
    ensureDir(dir);
    return dir;
}

bool ensureDir(const std::filesystem::path& path)
{
    try
    {
        if (!std::filesystem::exists(path))
        {
            return std::filesystem::create_directories(path);
        }
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Platform] Failed to create directory: " << e.what() << std::endl;
        return false;
    }
}

std::string getOS()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string getHostname()
{
#ifdef _WIN32
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf) / sizeof(buf[0]);
    if (GetComputerNameW(buf, &size))
    {
        // Convert wide to narrow via WideCharToMultiByte
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(size), result.data(), len, nullptr, nullptr);
        return result;
    }
#endif
    return "unknown";
}

void openFolderInExplorer(const std::filesystem::path& folder)
{
#ifdef _WIN32
    ShellExecuteW(nullptr, L"explore", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + folder.string() + "\"";
    std::system(cmd.c_str());
#elif defined(__linux__)
    std::string cmd = "xdg-open \"" + folder.string() + "\"";
    std::system(cmd.c_str());
#endif
}

bool addFirewallRule(const std::string& ruleName, uint16_t tcpPort, uint16_t udpPort)
{
#ifdef _WIN32
    std::wstring name(ruleName.begin(), ruleName.end());

    // Build a single cmd /c command that deletes old rules and adds new ones.
    // One UAC prompt covers all operations.
    std::wstring cmd = L"/c "
        L"netsh advfirewall firewall delete rule name=\"" + name + L"\" >nul 2>&1 & "
        L"netsh advfirewall firewall add rule name=\"" + name + L"\" "
        L"dir=in action=allow protocol=tcp localport=" + std::to_wstring(tcpPort) + L" enable=yes";

    if (udpPort > 0)
    {
        cmd += L" & netsh advfirewall firewall add rule name=\"" + name + L" UDP\" "
               L"dir=in action=allow protocol=udp localport=" + std::to_wstring(udpPort) + L" enable=yes";
    }

    HINSTANCE result = ShellExecuteW(nullptr, L"runas", L"cmd.exe",
                                      cmd.c_str(), nullptr, SW_HIDE);
    return reinterpret_cast<intptr_t>(result) > 32;
#else
    (void)ruleName;
    (void)tcpPort;
    (void)udpPort;
    return false;
#endif
}

} // namespace MR
