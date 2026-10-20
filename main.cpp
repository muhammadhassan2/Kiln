// Shader Studio — live GLSL editor
// GLFW + Dear ImGui (docking) + OpenGL 4.1 core behind a small RHI (src/rhi).

#include "app/App.h"
#include "app/Theme.h"
#include "rhi/gl/GLHeaders.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <nfd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Ctrl+C / the IDE's Stop button: leave the loop normally so the session is autosaved.
std::atomic<bool> gStopRequested{false};
void onSignal(int) { gStopRequested = true; }

void glfwError(int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); }

void onDrop(GLFWwindow* window, int count, const char** paths) {
    if (auto* app = static_cast<app::App*>(glfwGetWindowUserPointer(window))) app->onFilesDropped(count, paths);
}

}  // namespace

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfwError);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (!glfwInit()) return EXIT_FAILURE;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // required on macOS
#if defined(__APPLE__)
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1600, 960, "Shader Studio", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

#if !defined(__APPLE__)
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::fprintf(stderr, "Failed to initialise GLEW\n");
        return EXIT_FAILURE;
    }
    glGetError();  // GLEW can leave GL_INVALID_ENUM on core profiles
#endif

    NFD_Init();  // native file dialogs (Cocoa on macOS, GTK/portal on Linux, COM on Windows)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // App points this at its config folder

    app::applyTheme();
    const app::Fonts fonts = app::loadFonts();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410 core");

    int exitCode = EXIT_SUCCESS;
    {
        app::App application(window, fonts);
        glfwSetWindowUserPointer(window, &application);
        glfwSetDropCallback(window, onDrop);

        // Optional: open a shader / images passed on the command line.
        application.handleArgs(argc, argv);

        // Smoke-test hook: SHADER_STUDIO_FRAMES=N quits after N frames (used by CI).
        int frameLimit = -1;
        if (const char* f = std::getenv("SHADER_STUDIO_FRAMES")) frameLimit = std::atoi(f);

        while (!glfwWindowShouldClose(window) && !application.quitRequested() && frameLimit != 0 && !gStopRequested) {
            glfwPollEvents();
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                glfwWaitEventsTimeout(0.1);
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            application.update();
            application.drawUI();

            ImGui::Render();
            int fbw = 0, fbh = 0;
            glfwGetFramebufferSize(window, &fbw, &fbh);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, fbw, fbh);
            glClearColor(0.055f, 0.059f, 0.071f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            application.presentSecondary();  // optional second output window

            if (frameLimit > 0) --frameLimit;
        }
        application.shutdown();  // autosave the session, stop media processes
        exitCode = application.exitCode();
        glfwSetWindowUserPointer(window, nullptr);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    NFD_Quit();
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
