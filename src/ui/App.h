#pragma once
#include <memory>

namespace cs::ui {

// Owns every service, runs the message loop and the ImGui frame, and turns UI actions,
// hotkeys, tray clicks and engine events into calls on the right service.
class App {
public:
    App();
    ~App();
    bool Init(void* hinstance, bool startHidden);
    int Run();
    void Shutdown();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs::ui
