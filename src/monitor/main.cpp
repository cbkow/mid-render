#include "monitor/monitor_app.h"
#include "monitor/ui/style.h"
#include "core/system_tray.h"
#include "core/single_instance.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.h>

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <cstdlib>
#include <crtdbg.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

static void invalidParameterHandler(
    const wchar_t* expression, const wchar_t* function,
    const wchar_t* file, unsigned int line, uintptr_t /*reserved*/)
{
    std::wcerr << L"[CRT] Invalid parameter: " << (expression ? expression : L"(null)")
               << L" in " << (function ? function : L"(null)")
               << L" at " << (file ? file : L"(null)")
               << L":" << line << std::endl;
}
#endif

static void glfwErrorCallback(int error, const char* description)
{
    std::cerr << "[GLFW] Error " << error << ": " << description << std::endl;
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _set_invalid_parameter_handler(invalidParameterHandler);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif

    // Parse CLI flags
    bool startMinimized = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--minimized") == 0)
            startMinimized = true;
    }

    // --- Single Instance Check ---
    MR::SingleInstance singleInstance("MidRenderMonitor");
    if (!singleInstance.isFirst())
    {
        singleInstance.signalExisting();
        return 0;
    }

    // --- GLFW init ---
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

    // GL 3.3 Core
    const char* glslVersion = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    if (startMinimized)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    std::string windowTitle = std::string("MidRender Monitor v") + APP_VERSION;
    GLFWwindow* window = glfwCreateWindow(1280, 720, windowTitle.c_str(), nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // No hardware vsync — manual frame limiter below.
                         // vsync(1) can freeze on Win10 with remote desktop tools
                         // (Jump Desktop, RDP, etc.) where SwapBuffers blocks
                         // waiting for a vblank that never arrives.
                         // DWM compositing prevents tearing for windowed apps anyway.

    // --- GLAD ---
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)))
    {
        std::cerr << "Failed to initialize OpenGL loader" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --- ImGui ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;

    MR::loadFonts();
    MR::setupStyle();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    MR::enableDarkTitleBar(window);

    // Set window icon from embedded resource
#ifdef _WIN32
    {
        HICON iconBig = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (iconBig)
        {
            HWND hwnd = glfwGetWin32Window(window);
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconBig));
        }
    }
#endif

    // --- NFD ---
    NFD_Init();

    // --- App ---
    MR::MonitorApp app;
    if (!app.init())
    {
        std::cerr << "Failed to initialize MonitorApp" << std::endl;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    // --- X button hides window ---
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        glfwHideWindow(w);
    });

    // --- System tray ---
    MR::SystemTray tray;
    tray.init();

    tray.onShowWindow = [&]() {
        glfwShowWindow(window);
        glfwFocusWindow(window);
    };

    tray.onStopResume = [&]() {
        if (app.nodeState() == MR::NodeState::Active)
            app.setNodeState(MR::NodeState::Stopped);
        else
            app.setNodeState(MR::NodeState::Active);
    };

    tray.onExit = [&]() {
        app.requestExit();
    };

    // --- Main loop (manual ~60fps cap, no hardware vsync) ---
    constexpr auto frameBudget = std::chrono::microseconds(16'667); // ~60fps
    auto frameStart = std::chrono::steady_clock::now();

    while (!app.shouldExit())
    {
        frameStart = std::chrono::steady_clock::now();

        glfwPollEvents();
        app.update();

        bool visible = glfwGetWindowAttrib(window, GLFW_VISIBLE) != 0;

        if (app.isExitPending() && !visible)
        {
            glfwShowWindow(window);
            glfwFocusWindow(window);
            visible = true;
        }

        tray.setIcon(app.trayState());
        tray.setTooltip(app.trayTooltip());
        tray.setStatusText(app.trayStatusText());
        tray.setNodeActive(app.nodeState() == MR::NodeState::Active);

        if (visible)
        {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            app.renderUI();

            ImGui::Render();
            int displayW, displayH;
            glfwGetFramebufferSize(window, &displayW, &displayH);
            glViewport(0, 0, displayW, displayH);
            glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                GLFWwindow* backupCtx = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backupCtx);
            }

            glfwSwapBuffers(window);

            // Sleep remainder of frame budget to cap at ~60fps
            auto elapsed = std::chrono::steady_clock::now() - frameStart;
            if (elapsed < frameBudget)
                std::this_thread::sleep_for(frameBudget - elapsed);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // --- Cleanup ---
    tray.shutdown();
    app.shutdown();
    NFD_Quit();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
