// macOS: receive "open these files" requests from Finder and the Dock.
//
// GLFW owns the NSApplication delegate (class GLFWApplicationDelegate) and does not forward
// open-document events, so we add -application:openURLs: to that class at runtime. AppKit checks
// respondsToSelector: when the event arrives, so adding the method before glfwInit() is enough
// to also catch files passed at launch.

#import <Cocoa/Cocoa.h>
#include <objc/runtime.h>

#include "platform/OpenFiles.h"

#include <mutex>
#include <utility>

namespace {

std::mutex gMutex;
std::vector<std::string> gFiles;

void openURLs(id /*self*/, SEL /*cmd*/, NSApplication* /*app*/, NSArray<NSURL*>* urls) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (NSURL* url in urls)
        if ([url isFileURL] && url.path) gFiles.emplace_back(url.path.UTF8String);
}

}  // namespace

namespace platform {

void installOpenFileHandler() {
    Class cls = objc_getClass("GLFWApplicationDelegate");
    if (!cls) return;  // GLFW renamed its delegate: file opening from Finder is simply unavailable
    class_addMethod(cls, @selector(application:openURLs:), reinterpret_cast<IMP>(openURLs), "v@:@@");
}

std::vector<std::string> takeOpenedFiles() {
    std::lock_guard<std::mutex> lock(gMutex);
    return std::exchange(gFiles, {});
}

}  // namespace platform
