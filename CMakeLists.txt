cmake_minimum_required(VERSION 3.20)
project(MidRender VERSION 0.2.8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- FetchContent dependencies ---
include(FetchContent)

# GLFW 3.4
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ImGui (docking branch)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# Native File Dialog Extended
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.2.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nfd)

# cpp-httplib (HTTP mesh — Phase 2)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)  # Header-only mode
FetchContent_MakeAvailable(httplib)

# SQLiteCpp (Phase 3 — SQLite wrapper)
FetchContent_Declare(
    SQLiteCpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG        3.3.2
    GIT_SHALLOW    TRUE
)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_DOXYGEN OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SQLiteCpp)

# --- GLAD (pre-generated, GL 3.3 Core) ---
add_library(glad STATIC external/glad/src/gl.c)
target_include_directories(glad PUBLIC external/glad/include)

# --- ImGui (manual target — no upstream CMakeLists.txt) ---
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw glad)

# --- MidRender Monitor ---
add_executable(midrender
    src/core/platform.cpp
    src/core/node_identity.cpp
    src/core/ipc_server.cpp
    src/core/monitor_log.cpp
    src/core/system_tray.cpp
    src/core/single_instance.cpp
    src/core/net_utils.cpp
    src/core/http_server.cpp
    src/core/udp_notify.cpp
    src/monitor/peer_manager.cpp
    src/monitor/main.cpp
    src/monitor/monitor_app.cpp
    src/monitor/agent_supervisor.cpp
    src/monitor/template_manager.cpp
    src/monitor/render_coordinator.cpp
    src/monitor/database_manager.cpp
    src/monitor/dispatch_manager.cpp
    src/monitor/farm_init.cpp
    src/monitor/submission_watcher.cpp
    src/monitor/ui/dashboard.cpp
    src/monitor/ui/settings_panel.cpp
    src/monitor/ui/node_panel.cpp
    src/monitor/ui/job_list_panel.cpp
    src/monitor/ui/job_detail_panel.cpp
    src/monitor/ui/log_panel.cpp
    src/monitor/ui/farm_cleanup_dialog.cpp
    src/monitor/ui/style.cpp
    resources/midrender.rc
)

target_include_directories(midrender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(midrender PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    CPPHTTPLIB_THREAD_POOL_COUNT=4
)

target_link_libraries(midrender PRIVATE
    imgui
    glad
    nlohmann_json::nlohmann_json
    httplib::httplib
    SQLiteCpp
    nfd
    opengl32
    shell32
    ole32
    Rpcrt4
    dwmapi
    Advapi32
    Ws2_32
    Iphlpapi
)

# Copy resources/ next to the executable at build time
add_custom_command(TARGET midrender POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:midrender>/resources"
    COMMENT "Copying resources to build output"
)

# MSVC-specific settings
if(MSVC)
    target_compile_options(midrender PRIVATE /W4)
    # Windows subsystem (no console window)
    set_target_properties(midrender PROPERTIES WIN32_EXECUTABLE TRUE)
    set_target_properties(midrender PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()
