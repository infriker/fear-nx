#pragma once

// Reuse the validated 0.10 Windows translation. GCC requires alias attributes
// after the extern-C declaration, whereas the stock generator places them
// before it. This matches the Switch SDK's patched pch_h.inja template.
#if defined(__GNUC__) && !defined(__clang__)
#define DEFINE_REX_FUNC(name)                                       \
  extern "C" REX_FUNC(name)                                        \
      __attribute__((alias("__imp__" #name), weak, noinline));       \
  REX_EXTERN(__imp__##name)
#endif

#include "generated/default/fear_xbox360_pch.h"
#if defined(FEAR_NX_CPU_ADAPTER)
#include "cpu/fear_cpu_guest.h"
#endif
