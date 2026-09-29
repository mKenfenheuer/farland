// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "input_thread.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using farland::app::Desktop;
using farland::app::DesktopInput;
using farland::app::InputThread;
namespace proto = farland::proto;

namespace {

constexpr std::uint16_t scancode_a = 0x1E;
constexpr std::uint32_t key_a = 30;  ///< KEY_A

/// Records what reaches the compositor, and on which thread.
class RecordingSink final : public farland::platform::InputSink {
public:
    void key(std::uint32_t code, bool pressed) override { add(std::format("key {} {}", code, pressed ? 1 : 0)); }
    void pointer_motion_absolute(double x, double y) override { add(std::format("abs {} {}", x, y)); }
    void pointer_motion_relative(double /*dx*/, double /*dy*/) override {}
    void button(std::uint32_t code, bool pressed) override { add(std::format("button {} {}", code, pressed ? 1 : 0)); }
    void scroll_discrete(std::int32_t /*x*/, std::int32_t /*y*/) override {}
    void text(char32_t /*codepoint*/) override {}
    void flush() override {}

    void add(std::string what)
    {
        const std::scoped_lock lock(mutex_);
        calls_.push_back(std::move(what));
        threads_.push_back(std::this_thread::get_id());
        changed_.notify_all();
    }
    /// Waits until `count` calls arrived; all of them then.
    std::vector<std::string> wait_for(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::seconds(5), [&] { return calls_.size() >= count; });
        return calls_;
    }
    std::vector<std::thread::id> threads()
    {
        const std::scoped_lock lock(mutex_);
        return threads_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> calls_;
    std::vector<std::thread::id> threads_;
};

class FakeInput final : public DesktopInput {
public:
    explicit FakeInput(RecordingSink& sink) : sink_(sink) {}
    [[nodiscard]] farland::platform::InputSink& sink() override { return sink_; }
    [[nodiscard]] std::vector<int> fds() const override { return {}; }
    void dispatch() override {}
    [[nodiscard]] bool closed() const override { return false; }
    /// Stands for EiInput::set_outputs(): a change the desktop makes.
    void set_outputs(std::string name) { sink_.add("outputs " + name); }

private:
    RecordingSink& sink_;
};

/// A desktop that lets its input go, and changes it now and then.
class FakeDesktop final : public Desktop {
public:
    explicit FakeDesktop(bool movable)
    {
        if (movable) {
            keep_input(std::make_unique<FakeInput>(sink));
        }
    }
    [[nodiscard]] farland::platform::FrameSource& frames() override { std::abort(); }
    [[nodiscard]] farland::platform::CursorSource* cursor() override { return nullptr; }
    [[nodiscard]] farland::platform::InputSink& input() override { return sink; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override { return {}; }
    void dispatch() override {}
    [[nodiscard]] bool closed() const override { return false; }

    void move_screens(std::string name)
    {
        change_input([name](DesktopInput& input) { static_cast<FakeInput&>(input).set_outputs(name); });
    }

    RecordingSink sink;
};

std::vector<proto::InputEvent> key(bool pressed)
{
    return {proto::KeyboardEvent{pressed ? std::uint16_t{0} : proto::kbd_flags::release, scancode_a}};
}

}  // namespace

TEST_CASE("Input runs on a thread of its own, in order with the desktop's changes to it")
{
    FakeDesktop desktop(true);
    auto thread = InputThread::start(desktop);
    REQUIRE(thread);

    desktop.move_screens("left");  // while the session holds the input: on its thread
    thread->translate(key(true));
    thread->set_geometry(0, 0, 0, 0);
    thread->translate(std::vector<proto::InputEvent>{proto::MouseEvent{proto::ptr_flags::move, 10, 20}});
    thread->translate(key(false));

    const auto calls = desktop.sink.wait_for(4);
    CHECK(calls == std::vector<std::string>{"outputs left", std::format("key {} 1", key_a), "abs 10 20",
                                            std::format("key {} 0", key_a)});
    for (const auto id : desktop.sink.threads()) {
        CHECK(id != std::this_thread::get_id());
    }
}

TEST_CASE("Stopping the input thread releases what is held and gives the input back")
{
    FakeDesktop desktop(true);
    auto thread = InputThread::start(desktop);
    REQUIRE(thread);
    thread->translate(key(true));
    CHECK(desktop.sink.wait_for(1).size() == 1);

    thread->stop();
    auto calls = desktop.sink.wait_for(2);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1] == std::format("key {} 0", key_a));  // never left held on the desktop

    // Input after the end goes nowhere; the desktop has its input again and
    // changes it directly.
    thread->translate(key(true));
    desktop.move_screens("right");
    calls = desktop.sink.wait_for(3);
    CHECK(calls.back() == "outputs right");
    CHECK(desktop.sink.threads().back() == std::this_thread::get_id());

    // And the next session can take it.
    thread = InputThread::start(desktop);
    CHECK(thread);
}

TEST_CASE("A desktop whose input cannot leave its thread gets no input thread")
{
    FakeDesktop desktop(false);
    CHECK_FALSE(InputThread::start(desktop));
}
