cmake_minimum_required(VERSION 3.24)
project(Shader_Simulation LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Some third-party CMakeLists still declare very old minimums; CMake 4 rejects them without this.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

include(FetchContent)

# ---------------------------------------------------------------- dependencies (pinned)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE)

FetchContent_Declare(texteditor
    GIT_REPOSITORY https://github.com/goossens/ImGuiColorTextEdit.git
    GIT_TAG        b12d713e77e8edddff1e69973f67085ac9cfaf74)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL     OFF CACHE BOOL "" FORCE)
set(NFD_WAYLAND     OFF CACHE BOOL "" FORCE)  # GLFW here is X11-only on Linux
FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG        v1.4.1
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES "")  # submodules are only for the Wayland backend

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE)

# glslang + SPIRV-Cross: GLSL ES validation and GLSL -> MSL conversion.
set(ENABLE_OPT              OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL             OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER      OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        16.6.0
    GIT_SHALLOW    TRUE)

set(SPIRV_CROSS_CLI            OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS   OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL    OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP     OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API   OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL    OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL   ON  CACHE BOOL "" FORCE)
FetchContent_Declare(spirvcross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG        vulkan-sdk-1.4.363.0
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(glfw imgui texteditor stb nfd json glslang spirvcross)

if (NOT APPLE)
    set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ONLY_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(glew
        GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
        GIT_TAG        glew-cmake-2.3.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(glew)
endif ()

# ---------------------------------------------------------------- third-party static lib
add_library(thirdparty STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${texteditor_SOURCE_DIR}/TextEditor.cpp
    ${texteditor_SOURCE_DIR}/TextDiff.cpp
    src/third_party/stb_impl.cpp)

target_include_directories(thirdparty PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${texteditor_SOURCE_DIR}
    ${stb_SOURCE_DIR})

target_link_libraries(thirdparty PUBLIC glfw nfd nlohmann_json::nlohmann_json
    glslang glslang-default-resource-limits SPIRV spirv-cross-msl spirv-cross-glsl spirv-cross-core)

if (APPLE)
    target_compile_definitions(thirdparty PUBLIC GL_SILENCE_DEPRECATION)
    target_link_libraries(thirdparty PUBLIC "-framework OpenGL")
else ()
    target_link_libraries(thirdparty PUBLIC libglew_static)
    find_package(OpenGL REQUIRED)
    target_link_libraries(thirdparty PUBLIC OpenGL::GL)
endif ()

find_package(Threads REQUIRED)
target_link_libraries(thirdparty PUBLIC Threads::Threads)

# ---------------------------------------------------------------- app
add_executable(Shader_Simulation
    main.cpp
    src/ai/Assistant.cpp
    src/app/App.cpp
    src/app/AppFiles.cpp
    src/app/AppPreview.cpp
    src/app/AppTextures.cpp
    src/app/AppTimeline.cpp
    src/app/AppTools.cpp
    src/app/AppUI.cpp
    src/app/ExportPlan.cpp
    src/app/OutputWindow.cpp
    src/app/Templates.cpp
    src/app/Theme.cpp
    src/media/AudioSource.cpp
    src/media/Process.cpp
    src/media/VideoSource.cpp
    src/rhi/gl/GLDevice.cpp
    src/shader/Annotations.cpp
    src/shader/CrossCompile.cpp
    src/shader/Diagnostics.cpp
    src/shader/Translate.cpp
    src/textures/TextureLibrary.cpp)

target_include_directories(Shader_Simulation PRIVATE src)
target_link_libraries(Shader_Simulation PRIVATE thirdparty)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(Shader_Simulation PRIVATE -Wall -Wextra)
endif ()

# Debug builds (CLion's default) still optimise the third-party code: ImGui, the code editor,
# glslang and SPIRV-Cross are many times slower at -O0, which makes big shaders feel laggy
# (typing, compiling, mobile check). Your own code under src/ stays fully debuggable.
option(SHADER_STUDIO_FAST_DEPS "Optimise third-party libraries in Debug builds" ON)
if (SHADER_STUDIO_FAST_DEPS AND NOT MSVC)  # MSVC: /O2 clashes with Debug's /RTC1 checks
    set(_fast_flag -O2)
    foreach (_t thirdparty glfw glslang SPIRV MachineIndependent GenericCodeGen OSDependent
                glslang-default-resource-limits spirv-cross-core spirv-cross-glsl spirv-cross-msl nfd)
        if (TARGET ${_t})
            get_target_property(_type ${_t} TYPE)
            if (NOT _type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${_t} PRIVATE $<$<CONFIG:Debug>:${_fast_flag}>)
            endif ()
        endif ()
    endforeach ()
    # The shader preprocessing helpers run on every compile; keep them fast too.
    set_source_files_properties(src/shader/Translate.cpp src/shader/Annotations.cpp
        PROPERTIES COMPILE_OPTIONS $<$<CONFIG:Debug>:${_fast_flag}>)
endif ()

# ---------------------------------------------------------------- tests (no GL/UI needed)
enable_testing()
add_executable(export_plan_tests tests/ExportPlanTests.cpp src/app/ExportPlan.cpp)
target_include_directories(export_plan_tests PRIVATE src)
add_test(NAME export_plan COMMAND export_plan_tests)
