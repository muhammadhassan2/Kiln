#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace app {

// A second, chrome-free window that shows the output texture (for a projector / second monitor).
// Shares the main GL context's objects; owns its own VAO and blit program.
class OutputWindow {
public:
    ~OutputWindow();

    // monitor < 0 = windowed, else fullscreen on that monitor index.
    bool open(GLFWwindow* share, int monitor);
    void close();
    bool isOpen() const { return window_ != nullptr; }
    int monitor() const { return monitor_; }

    // Draws `glTexture` letterboxed into the window, then restores `mainContext`.
    void present(std::uint64_t glTexture, int texW, int texH, GLFWwindow* mainContext);

    static std::vector<std::string> monitorNames();

private:
    GLFWwindow* window_ = nullptr;
    unsigned vao_ = 0, program_ = 0;
    int monitor_ = -1;
};

}  // namespace app
