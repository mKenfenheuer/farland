// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "desktop.hpp"
#include "wake_pipe.hpp"

#include <farland/channels/rdpei_server.hpp>
#include <farland/platform/input_translator.hpp>
#include <farland/proto/input.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace farland::app {

/// The session's keyboard, mouse and touch, on a thread that does nothing
/// else: it owns the desktop's input connection for as long as the session
/// runs, translates what the client sends and hands it to the compositor, and
/// services the connection. Input thus reaches the desktop while the
/// session's other threads are busy (a frame encoding, audio going out).
///
/// Everything else posts to it and returns at once; the jobs run in the order
/// they were posted.
class InputThread {
public:
    /// Takes `desktop`'s input connection (Desktop::take_input()) and starts
    /// the thread; null when the desktop's input cannot leave its thread.
    /// `desktop` must outlive the result.
    [[nodiscard]] static std::unique_ptr<InputThread> start(Desktop& desktop);
    /// stop().
    ~InputThread();
    InputThread(const InputThread&) = delete;
    InputThread& operator=(const InputThread&) = delete;
    InputThread(InputThread&&) = delete;
    InputThread& operator=(InputThread&&) = delete;

    /// The events of one input PDU.
    void translate(std::vector<proto::InputEvent> events);
    /// The contacts of one touch or pen frame.
    void translate(std::vector<channels::rdpei::Contact> contacts);
    /// InputTranslator::set_geometry(), in order with the input around it.
    void set_geometry(std::uint32_t client_width, std::uint32_t client_height, std::uint32_t desktop_width,
                      std::uint32_t desktop_height, std::int32_t desktop_x = 0, std::int32_t desktop_y = 0);
    /// Releases every key, button and touch still held, ends the thread and
    /// gives the input connection back to the desktop, with the changes the
    /// desktop made to it meanwhile. Once; the destructor does it otherwise.
    void stop();

private:
    using Job = std::function<void()>;

    explicit InputThread(Desktop& desktop) : desktop_(desktop) {}
    /// A change from the desktop (Desktop::InputChange), from any thread.
    void post(Desktop::InputChange change);
    void enqueue(Job job);
    void run();

    Desktop& desktop_;
    std::unique_ptr<DesktopInput> input_;  ///< the thread's until stop()
    std::optional<platform::InputTranslator> translator_;
    WakePipe wake_;
    std::mutex mutex_;
    std::deque<Job> jobs_;
    /// Changes the desktop posted after the thread stopped taking jobs.
    std::deque<Desktop::InputChange> late_changes_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace farland::app
