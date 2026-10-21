// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <rex/input/input_system.h>

namespace fear_nx::input {
std::unique_ptr<rex::input::InputSystem> CreateInputSystem(bool tool_mode);
void VerifyGyroHooks(uint8_t* base);
void SetGyroFocus(bool focused);
void ShutdownGyro();
}  // namespace fear_nx::input
