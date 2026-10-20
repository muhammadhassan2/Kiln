#pragma once

#include <string>
#include <vector>

// Files the operating system asks the app to open: double-clicking a .kiln project in Finder,
// "Open With", or dropping files on the Dock icon. macOS delivers these as events, not argv.
namespace platform {

#if defined(__APPLE__)
// Call before glfwInit(): files passed at launch arrive while GLFW starts AppKit.
void installOpenFileHandler();
// Paths received since the last call (thread-safe).
std::vector<std::string> takeOpenedFiles();
#else
inline void installOpenFileHandler() {}
inline std::vector<std::string> takeOpenedFiles() { return {}; }
#endif

}  // namespace platform
