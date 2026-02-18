#include "monitor/farm_init.h"
#include "core/config.h"
#include "core/platform.h"
#include "core/monitor_log.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace MR {

namespace fs = std::filesystem;

static std::string getExeDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
#else
    return ".";
#endif
}

static fs::path findBundledTemplatesDir()
{
    fs::path dir = fs::path(getExeDir()) / "resources" / "templates";
    if (fs::is_directory(dir))
        return dir;
    return {};
}

static fs::path findBundledPluginsDir()
{
    fs::path dir = fs::path(getExeDir()) / "resources" / "plugins";
    if (fs::is_directory(dir))
        return dir;
    return {};
}

static void copyExampleTemplates(const fs::path& farmPath)
{
    auto bundled = findBundledTemplatesDir();
    if (bundled.empty())
    {
        MonitorLog::instance().warn("farm", "No bundled templates found, skipping example copy");
        return;
    }

    auto destDir = farmPath / "templates" / "examples";
    std::error_code ec;

    // Copy top-level *.json files
    for (auto& entry : fs::directory_iterator(bundled, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            auto dest = destDir / entry.path().filename();
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
            if (!ec)
                MonitorLog::instance().info("farm", "Copied template: " + entry.path().filename().string());
        }
    }

    // Copy plugins/*.json into templates/plugins/ (separate from examples so
    // they don't appear in the Monitor's template picker — only DCC plugins scan this dir)
    auto pluginTemplatesDir = bundled / "plugins";
    if (fs::is_directory(pluginTemplatesDir, ec))
    {
        auto pluginDestDir = farmPath / "templates" / "plugins";
        fs::create_directories(pluginDestDir, ec);

        for (auto& entry : fs::directory_iterator(pluginTemplatesDir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
            {
                auto dest = pluginDestDir / entry.path().filename();
                fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    MonitorLog::instance().info("farm", "Copied plugin template: " + entry.path().filename().string());
            }
        }
    }
}

static void copyPlugins(const fs::path& farmPath)
{
    auto bundled = findBundledPluginsDir();
    if (bundled.empty())
    {
        MonitorLog::instance().warn("farm", "No bundled plugins found, skipping plugin copy");
        return;
    }

    std::error_code ec;
    for (auto& appDir : fs::directory_iterator(bundled, ec))
    {
        if (!appDir.is_directory(ec)) continue;

        auto destDir = farmPath / "plugins" / appDir.path().filename();
        fs::create_directories(destDir, ec);

        for (auto& entry : fs::directory_iterator(appDir.path(), ec))
        {
            if (!entry.is_regular_file(ec)) continue;
            auto dest = destDir / entry.path().filename();
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
            if (!ec)
                MonitorLog::instance().info("farm", "Copied plugin: " +
                    appDir.path().filename().string() + "/" + entry.path().filename().string());
        }
    }
}

FarmInit::Result FarmInit::init(const fs::path& farmPath, const std::string& nodeId)
{
    Result result;
    std::error_code ec;

    auto farmJsonPath = farmPath / "farm.json";
    bool hasFarmJson = fs::exists(farmJsonPath, ec);

    if (!hasFarmJson)
    {
        // First run — write farm.json + copy templates + plugins
        MonitorLog::instance().info("farm", "Creating farm.json at: " + farmPath.string());

        fs::create_directories(farmPath / "templates" / "examples", ec);
        fs::create_directories(farmPath / "plugins", ec);

        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        nlohmann::json farmJson = {
            {"_version", 1},
            {"protocol_version", PROTOCOL_VERSION},
            {"created_by", nodeId},
            {"created_at_ms", nowMs},
            {"last_example_update", APP_VERSION}
        };

        std::ofstream ofs(farmJsonPath);
        if (ofs.is_open())
        {
            ofs << farmJson.dump(2);
            ofs.close();
        }
        else
        {
            result.error = "Failed to write farm.json";
            return result;
        }

        copyExampleTemplates(farmPath);
        copyPlugins(farmPath);

        MonitorLog::instance().info("farm", "Farm initialized");
    }
    else
    {
        // Check if example templates need update
        try
        {
            std::ifstream ifs(farmJsonPath);
            nlohmann::json fj = nlohmann::json::parse(ifs);
            ifs.close();

            std::string lastUpdate;
            if (fj.contains("last_example_update"))
                lastUpdate = fj["last_example_update"].get<std::string>();

            if (lastUpdate != APP_VERSION)
            {
                MonitorLog::instance().info("farm",
                    "Updating examples (" + lastUpdate + " -> " + std::string(APP_VERSION) + ")");

                copyExampleTemplates(farmPath);
                copyPlugins(farmPath);

                fj["last_example_update"] = APP_VERSION;
                std::ofstream ofsUpdate(farmJsonPath);
                if (ofsUpdate.is_open())
                    ofsUpdate << fj.dump(2);
            }
        }
        catch (const std::exception& e)
        {
            MonitorLog::instance().warn("farm", "Failed to read farm.json: " + std::string(e.what()));
        }
    }

    result.success = true;
    return result;
}

} // namespace MR
