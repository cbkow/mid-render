#pragma once

namespace MR {

class MonitorApp; // forward

class NodePanel
{
public:
    void init(MonitorApp* app);
    void render();
    bool visible = true;

private:
    MonitorApp* m_app = nullptr;
};

} // namespace MR
