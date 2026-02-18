#pragma once

struct GLFWwindow;
struct ImFont;

namespace MR {

struct Fonts
{
    static inline ImFont* regular = nullptr;
    static inline ImFont* bold    = nullptr;
    static inline ImFont* italic  = nullptr;
    static inline ImFont* mono    = nullptr;
    static inline ImFont* icons   = nullptr;
};

void loadFonts();
void setupStyle();
void enableDarkTitleBar(GLFWwindow* window);

// Draw a custom panel header: bold title + close (X) button.
// Returns true if close was clicked.
bool panelHeader(const char* title, bool& visible);

} // namespace MR
