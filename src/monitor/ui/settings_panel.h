#pragma once

#include <string>

namespace MR {

class MonitorApp; // forward

class SettingsPanel
{
public:
    void init(MonitorApp* app);
    void render();

private:
    MonitorApp* m_app = nullptr;
    bool m_needsReload = true;

    // Editable copies of config values
    char m_syncRootBuf[512] = {};
    char m_tagsBuf[256] = {};
    int  m_httpPort = 8420;
    char m_ipOverrideBuf[64] = {};
    bool m_autoStartAgent = true;
    bool m_udpEnabled = true;
    int  m_udpPort = 4243;
    bool m_showNotifications = true;
    bool m_stagingEnabled = false;
    float m_fontScale = 1.0f;

    std::string m_savedSyncRoot;

    static constexpr float FONT_SCALE_SMALL  = 0.75f;
    static constexpr float FONT_SCALE_MEDIUM = 1.0f;
    static constexpr float FONT_SCALE_LARGE  = 1.25f;
    static constexpr float FONT_SCALE_XLARGE = 1.5f;

    void loadFromConfig();
    void applyToConfig();
    void drawFontSizeSection();
    void drawFontPreview();
};

} // namespace MR
