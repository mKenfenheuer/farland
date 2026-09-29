// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "input_thread.hpp"

#include <algorithm>
#include <poll.h>

namespace farland::app {

std::unique_ptr<InputThread> InputThread::start(Desktop& desktop)
{
    std::unique_ptr<InputThread> thread(new InputThread(desktop));
    // Changes the desktop makes from here on queue up, and run once the
    // thread does.
    auto input = desktop.take_input([raw = thread.get()](Desktop::InputChange change) { raw->post(std::move(change)); });
    if (!input) {
        return nullptr;
    }
    thread->input_ = std::move(input);
    thread->translator_.emplace(thread->input_->sink());
    thread->thread_ = std::thread([raw = thread.get()] { raw->run(); });
    return thread;
}

InputThread::~InputThread()
{
    stop();
}

void InputThread::translate(std::vector<proto::InputEvent> events)
{
    enqueue([this, events = std::move(events)] { translator_->translate(events); });
}

void InputThread::translate(std::vector<channels::rdpei::Contact> contacts)
{
    enqueue([this, contacts = std::move(contacts)] { translator_->translate(contacts); });
}

void InputThread::set_geometry(std::uint32_t client_width, std::uint32_t client_height, std::uint32_t desktop_width,
                               std::uint32_t desktop_height, std::int32_t desktop_x, std::int32_t desktop_y)
{
    enqueue([=, this] {
        translator_->set_geometry(client_width, client_height, desktop_width, desktop_height, desktop_x, desktop_y);
    });
}

void InputThread::post(Desktop::InputChange change)
{
    {
        const std::scoped_lock lock(mutex_);
        if (stopping_) {
            // The thread takes no more jobs: stop() applies it.
            late_changes_.push_back(std::move(change));
            return;
        }
        jobs_.push_back([this, change = std::move(change)] { change(*input_); });
    }
    wake_.notify();
}

void InputThread::stop()
{
    if (!thread_.joinable()) {
        return;
    }
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify();
    thread_.join();
    // The desktop posts its changes under the lock give_back_input() takes,
    // so none can come between the ones collected here and the input being
    // the desktop's again.
    desktop_.give_back_input(std::move(input_), [this](DesktopInput& input) {
        const std::scoped_lock lock(mutex_);
        for (const auto& change : late_changes_) {
            change(input);
        }
        late_changes_.clear();
    });
}

void InputThread::enqueue(Job job)
{
    {
        const std::scoped_lock lock(mutex_);
        if (stopping_) {
            return;  // the session is ending; no more input reaches the desktop
        }
        jobs_.push_back(std::move(job));
    }
    wake_.notify();
}

void InputThread::run()
{
    for (;;) {
        std::vector<pollfd> fds{pollfd{wake_.fd(), POLLIN, 0}};
        for (const int fd : input_->fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
        wake_.drain();
        std::deque<Job> jobs;
        bool stopping = false;
        {
            const std::scoped_lock lock(mutex_);
            jobs.swap(jobs_);
            stopping = stopping_;
        }
        for (auto& job : jobs) {
            job();
        }
        if (std::any_of(fds.begin() + 1, fds.end(), [](const pollfd& pfd) { return pfd.revents != 0; })) {
            input_->dispatch();
        }
        if (stopping) {
            // Nothing joined jobs_ since the swap: stopping_ was set by then.
            // Never leave keys held on a desktop the session lets go of.
            translator_->release_all();
            return;
        }
    }
}

}  // namespace farland::app
