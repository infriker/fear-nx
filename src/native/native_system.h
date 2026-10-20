// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Project8Recomp's generic Xenos Vulkan renderer (GPL-3.0),
// itself based on nfsmw-nx.
//
// Public entry points for F.E.A.R. NX's app-owned Xenos graphics system.
#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace fear_nx::native {

// Whether the nx_renderer cvar asks for the native renderer.
bool Enabled();

// Swaps the native renderer has presented since startup (0 when disabled).
uint64_t SwapsPresented();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace fear_nx::native
