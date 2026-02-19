#pragma once

#include <filesystem>
#include <string>

namespace MR {

// Returns platform app data directory: %LOCALAPPDATA%\MidRender\ on Windows
std::filesystem::path getAppDataDir();

// Creates directory tree if it doesn't exist. Returns true on success.
bool ensureDir(const std::filesystem::path& path);

// Returns "windows", "linux", or "macos"
std::string getOS();

// Returns machine hostname
std::string getHostname();

// Opens a folder in the platform file manager (Explorer, Finder, etc.)
void openFolderInExplorer(const std::filesystem::path& folder);

// Opens a URL in the default browser
void openUrl(const std::string& url);

// Requests elevated privileges to add Windows Firewall rules (TCP + optional UDP).
// Returns true if ShellExecute succeeded (user accepted UAC). No-op on non-Windows.
bool addFirewallRule(const std::string& ruleName, uint16_t tcpPort, uint16_t udpPort = 0);

} // namespace MR
