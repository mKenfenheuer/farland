// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

/// libFuzzer entry point, implemented once per fuzz target. Must not leak,
/// must not keep state between calls, and returns 0.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
