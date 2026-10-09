// ActorsWindow.h -- the "Actors..." window from the tray menu.
//
// Shows every .gifbpm in the actors folder as an animated tile with its BPM,
// in a scrollable grid. Click a tile to enable / disable that GIF (saved to
// <actors>/disabled.txt); "Add GIFs..." copies .gifbpm files into the folder
// and reloads them. Tiles are marked when the GIF is playing now, and when its
// tempo is too far from the music's to be picked (MAX_BPM_DIFF).
//
// The window runs on its own thread with its own message loop, so dragging it
// or using its file dialog never stalls the cursor animation. It talks to the
// main loop only through an ActorsBridge (Windows only).
#pragma once

#include "Dancer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

// Thread-safe mailbox between the cursor's main loop and the Actors window.
class ActorsBridge {
public:
    struct View {
        std::shared_ptr<dancer::Dancer> dancer;   // current dancer (may be loading)
        std::optional<int> bpm;                   // music tempo now
        double maxBpmDiff = 0;
        std::string actorsDir;
        int generation = 0;                       // bumps when the dancer is replaced
    };
    // main thread -> window
    void publish(std::shared_ptr<dancer::Dancer> d, std::optional<int> bpm,
                 double maxBpmDiff, const std::string& actorsDir);
    View view() const;
    // window -> main thread
    void requestDisabled(std::set<std::string> files);
    void requestReload();
    std::optional<std::set<std::string>> takeDisabled();
    bool takeReload();

private:
    mutable std::mutex m_;
    View v_;
    const dancer::Dancer* last_ = nullptr;
    std::optional<std::set<std::string>> disabled_;
    bool reload_ = false;
};

class ActorsWindow {
public:
    explicit ActorsWindow(ActorsBridge& bridge) : bridge_(bridge) {}
    ~ActorsWindow();                 // closes the window and joins its thread
    ActorsWindow(const ActorsWindow&) = delete;
    ActorsWindow& operator=(const ActorsWindow&) = delete;

    void show();                     // open, or bring to the front if open

    // window procedure (public for the static thunk)
    long long handle(void* hwnd, unsigned msg, unsigned long long wParam, long long lParam);

private:
    void run();
    void layout(int clientW);
    void paint(void* hdc, int w, int h);
    int  hitTest(int x, int y) const;   // tile index or -1
    void toggle(int index);
    void addFiles();
    void refreshView();

    ActorsBridge& bridge_;
    std::thread thread_;
    std::atomic<void*> hwnd_{nullptr};
    std::atomic<bool> running_{false};

    // window-thread state
    ActorsBridge::View view_;
    std::set<std::string> disabled_;    // window's copy (source of truth: disabled.txt)
    int  generation_ = -1;
    int  cols_ = 1, scrollY_ = 0, contentH_ = 0;
    void* font_ = nullptr;
    void* fontBold_ = nullptr;
    void* buttons_[3] = {nullptr, nullptr, nullptr};
    std::string status_;                // last action message
};
