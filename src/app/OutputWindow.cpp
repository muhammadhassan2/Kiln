#include "app/OutputWindow.h"

#include "rhi/gl/GLHeaders.h"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace app {

namespace {

const char* kVs = R"(#version 410 core
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFs = R"(#version 410 core
in vec2 v_uv;
uniform sampler2D u_src;
out vec4 o;
void main() { o = vec4(texture(u_src, v_uv).rgb, 1.0); }
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

}  // namespace

OutputWindow::~OutputWindow() { close(); }

std::vector<std::string> OutputWindow::monitorNames() {
    std::vector<std::string> names;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        std::string n = glfwGetMonitorName(monitors[i]) ? glfwGetMonitorName(monitors[i]) : "Monitor";
        if (mode) n += " (" + std::to_string(mode->width) + "x" + std::to_string(mode->height) + ")";
        names.push_back(n);
    }
    return names;
}

bool OutputWindow::open(GLFWwindow* share, int monitor) {
    close();
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* target = (monitor >= 0 && monitor < count) ? monitors[monitor] : nullptr;
    const GLFWvidmode* mode = target ? glfwGetVideoMode(target) : nullptr;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    window_ = glfwCreateWindow(mode ? mode->width : 1280, mode ? mode->height : 720, "Kiln — Output", target,
                               share);
    if (!window_) return false;
    monitor_ = target ? monitor : -1;

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(0);  // the main window already syncs to vblank
    glGenVertexArrays(1, &vao_);  // VAOs are not shared between contexts
    GLuint vs = compile(GL_VERTEX_SHADER, kVs), fs = compile(GL_FRAGMENT_SHADER, kFs);
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glfwMakeContextCurrent(share);
    return true;
}

void OutputWindow::close() {
    if (!window_) return;
    GLFWwindow* previous = glfwGetCurrentContext();
    glfwMakeContextCurrent(window_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteProgram(program_);
    vao_ = program_ = 0;
    glfwMakeContextCurrent(previous);
    glfwDestroyWindow(window_);
    window_ = nullptr;
}

void OutputWindow::present(std::uint64_t glTexture, int texW, int texH, GLFWwindow* mainContext) {
    if (!window_) return;
    if (glfwWindowShouldClose(window_)) {
        close();
        return;
    }
    glfwMakeContextCurrent(window_);
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (texW > 0 && texH > 0 && w > 0 && h > 0) {
        // Letterbox: keep the output's aspect ratio.
        const float s = std::min(static_cast<float>(w) / texW, static_cast<float>(h) / texH);
        const int vw = static_cast<int>(texW * s), vh = static_cast<int>(texH * s);
        glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(glTexture));
        glUniform1i(glGetUniformLocation(program_, "u_src"), 0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glUseProgram(0);
    }
    glfwSwapBuffers(window_);
    glfwMakeContextCurrent(mainContext);
}

}  // namespace app
