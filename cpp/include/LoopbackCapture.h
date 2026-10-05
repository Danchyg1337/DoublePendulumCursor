// LoopbackCapture.h -- records what the default output device is playing
// (WASAPI loopback), mixes it to mono and pushes it into an AudioRing.
//
// C++ twin of bpm_common.LoopbackCapture.run(): follows the default output
// device (checked every 3 s), reopens on errors after 1 s, and keeps the
// sample clock running through silence (Windows delivers no loopback packets
// while nothing plays, so silence is padded in by wall-clock time).
// Windows only.
#pragma once

#include "BeatWorker.h"

#include <atomic>
#include <thread>

class LoopbackCapture {
public:
    explicit LoopbackCapture(bpm::AudioRing& ring);
    ~LoopbackCapture();   // stops and joins the thread

    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

private:
    void run();
    bool captureOnce();   // one device session; false on error

    bpm::AudioRing&   ring_;
    std::atomic<bool> running_{true};
    std::thread       thread_;
};

// Runs BeatWorker::step() every BeatWorker::UPDATE_SEC on its own thread.
class BeatThread {
public:
    explicit BeatThread(bpm::BeatWorker& worker);
    ~BeatThread();
    BeatThread(const BeatThread&) = delete;
    BeatThread& operator=(const BeatThread&) = delete;
private:
    bpm::BeatWorker&  worker_;
    std::atomic<bool> running_{true};
    std::thread       thread_;
};
