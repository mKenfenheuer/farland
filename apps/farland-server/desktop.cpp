// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "desktop.hpp"

#include <farland/base/assert.hpp>

namespace farland::app {

std::unique_ptr<DesktopInput> Desktop::take_input(std::function<void(InputChange)> post)
{
    const std::scoped_lock lock(input_mutex_);
    if (!input_) {
        return nullptr;
    }
    post_input_ = std::move(post);
    return std::move(input_);
}

void Desktop::give_back_input(std::unique_ptr<DesktopInput> input, const InputChange& catch_up)
{
    const std::scoped_lock lock(input_mutex_);
    FARLAND_ASSERT(!input_ && input && input.get() == input_view_);
    catch_up(*input);
    post_input_ = nullptr;
    input_ = std::move(input);
}

void Desktop::keep_input(std::unique_ptr<DesktopInput> input)
{
    const std::scoped_lock lock(input_mutex_);
    FARLAND_ASSERT(!post_input_);
    input_ = std::move(input);
    input_view_ = input_.get();
}

void Desktop::change_input(const InputChange& change)
{
    const std::scoped_lock lock(input_mutex_);
    if (input_) {
        change(*input_);
    } else if (post_input_) {
        post_input_(change);
    }
}

void Desktop::drop_input()
{
    const std::scoped_lock lock(input_mutex_);
    FARLAND_ASSERT(!post_input_);
    input_.reset();
    input_view_ = nullptr;
}

std::vector<int> Desktop::input_fds() const
{
    const std::scoped_lock lock(input_mutex_);
    return input_ ? input_->fds() : std::vector<int>{};
}

platform::InputSink& Desktop::held_sink()
{
    const std::scoped_lock lock(input_mutex_);
    FARLAND_ASSERT(input_);
    return input_->sink();
}

void Desktop::dispatch_input()
{
    const std::scoped_lock lock(input_mutex_);
    if (input_) {
        input_->dispatch();
    }
}

}  // namespace farland::app
