#include <cstdint>
#include <cstdio>
#include <sys/stat.h>

#include <switch.h>

#include <rex/exception_handler.h>

#include "guest_memory_switch.h"

extern "C" uint32_t HorizonProbeRead(const uint32_t* address);
__asm__(
    ".text\n"
    ".balign 4\n"
    ".global HorizonProbeRead\n"
    ".type HorizonProbeRead, %function\n"
    "HorizonProbeRead:\n"
    "ldr w0, [x0]\n"
    "ret\n"
    ".size HorizonProbeRead, .-HorizonProbeRead\n");

namespace {

// Use the game's full virtual reservation, without preallocating its RAM.
constexpr size_t kProbeWindowSize = 0x120000000ull;
constexpr size_t kPageSize = 0x1000;
constexpr size_t kMirrorOffset = 0x10000;
constexpr size_t kFaultOffset = 0x20000;
constexpr uint32_t kInjectedFaultValue = 0xF34A51A7u;

struct HandlerState {
  uint64_t fault_address = 0;
  uint32_t hits = 0;
};

bool InstallProbeExceptionHandler(rex::arch::Exception* exception, void* opaque) {
  auto* state = static_cast<HandlerState*>(opaque);
  if (exception->code() != rex::arch::Exception::Code::kAccessViolation ||
      exception->fault_address() != state->fault_address ||
      exception->access_violation_operation() !=
          rex::arch::Exception::AccessViolationOperation::kRead) {
    return false;
  }

  // The assembly probe's first instruction loads W0 from the requested address.
  // Supply the test value in the saved context and resume after that instruction.
  exception->ModifyXRegister(0) = kInjectedFaultValue;
  exception->set_resume_pc(exception->pc() + 4);
  ++state->hits;
  return true;
}


bool OpenProbeLog() {
  mkdir("sdmc:/switch", 0777);
  mkdir("sdmc:/switch/fear-nx", 0777);
  if (!std::freopen("sdmc:/switch/fear-nx/horizon-probe.log", "w", stderr)) {
    std::printf("[probe] could not open sdmc:/switch/fear-nx/horizon-probe.log\n");
    std::fflush(stdout);
    return false;
  }
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  return true;
}

void LogResult(const char* name, Result result) {
  std::fprintf(stderr, "[probe] %s result=0x%08X\n", name, unsigned(result));
}

bool RunAliasProbe(uint8_t* base) {
  auto* mirror = base + kMirrorOffset;
  if (!RexGmAddView(base, 0, kPageSize)) {
    std::fprintf(stderr, "[probe] FAIL RexGmAddView(primary)\n");
    return false;
  }
  if (!RexGmAddView(mirror, 0, kPageSize)) {
    std::fprintf(stderr, "[probe] FAIL RexGmAddView(mirror)\n");
    return false;
  }
  if (!RexGmCommit(0, kPageSize, REX_GM_WRITE)) {
    std::fprintf(stderr, "[probe] FAIL RexGmCommit result=0x%08X\n",
                 unsigned(RexGmLastResult()));
    return false;
  }
  if (!RexGmFaultIn(reinterpret_cast<uint64_t>(base))) {
    std::fprintf(stderr, "[probe] FAIL RexGmFaultIn(primary)\n");
    return false;
  }
  if (!RexGmFaultIn(reinterpret_cast<uint64_t>(mirror))) {
    std::fprintf(stderr, "[probe] FAIL RexGmFaultIn(mirror)\n");
    return false;
  }

  constexpr uint64_t kPrimaryPattern = 0xA1B2C3D4E5F60718ull;
  constexpr uint64_t kMirrorPattern = 0x1029384756ABCDEFll;
  *reinterpret_cast<volatile uint64_t*>(base) = kPrimaryPattern;
  if (*reinterpret_cast<volatile uint64_t*>(mirror) != kPrimaryPattern) {
    std::fprintf(stderr, "[probe] FAIL mirror did not observe primary write\n");
    return false;
  }
  *reinterpret_cast<volatile uint64_t*>(mirror) = kMirrorPattern;
  if (*reinterpret_cast<volatile uint64_t*>(base) != kMirrorPattern) {
    std::fprintf(stderr, "[probe] FAIL primary did not observe mirror write\n");
    return false;
  }

  std::fprintf(stderr,
               "[probe] PASS MapProcessCodeMemory + MapProcessMemory alias read/write\n");
  return true;
}

bool RunCustomDataAbortProbe(HandlerState& state) {
  const uint32_t loaded =
      HorizonProbeRead(reinterpret_cast<const uint32_t*>(state.fault_address));

  if (state.hits != 1 || loaded != kInjectedFaultValue) {
    std::fprintf(stderr,
                 "[probe] FAIL custom DataAbort handler hits=%u value=0x%08X expected=0x%08X\n",
                 state.hits, loaded, kInjectedFaultValue);
    return false;
  }
  std::fprintf(stderr,
               "[probe] PASS custom DataAbort handler hit=1 resume-PC and X0 writeback\n");
  return true;
}

bool RunPhysicalProbe(uint8_t* base, const char* layout, size_t count,
                      size_t stride, size_t physical_offset, size_t mirror_offset) {
  const size_t kCount = count;
  const size_t kStride = stride;
  const size_t kPhysicalOffset = physical_offset;
  const size_t kSpan = kCount * kStride;
  constexpr uint64_t kPattern = 0xF34A260000000000ull;
  auto* physical = base + kPhysicalOffset;
  auto* mirror = base + mirror_offset;
  const size_t initial_committed = RexGmCommittedBytes();
  const size_t initial_mapped = RexGmMappedBytes();
  auto fail = [layout](const char* phase, size_t index) {
    std::fprintf(stderr, "[probe] FAIL %s phase=%s index=%zu result=0x%08X\n",
                 layout, phase, index, unsigned(RexGmLastResult()));
    return false;
  };
  if (!RexGmAddView(physical, kPhysicalOffset, kSpan) ||
      !RexGmAddView(mirror, kPhysicalOffset, kSpan)) {
    return fail("add-views", 0);
  }
  for (size_t i = 0; i < kCount; ++i) {
    const size_t delta = i * kStride;
    if (!RexGmCommit(kPhysicalOffset + delta, kPageSize, REX_GM_WRITE))
      return fail("commit", i);
    if (!RexGmFaultIn(reinterpret_cast<uint64_t>(physical + delta)))
      return fail("map-primary", i);
    if (!RexGmFaultIn(reinterpret_cast<uint64_t>(mirror + delta)))
      return fail("map-mirror", i);
    *reinterpret_cast<volatile uint64_t*>(physical + delta) = kPattern | i;
    if (*reinterpret_cast<volatile uint64_t*>(mirror + delta) != (kPattern | i)) {
      return fail("alias", i);
    }
    // Reserved holes must remain uncommitted; batching may not open them.
    if (kStride > kPageSize &&
        (RexGmShadowFor(reinterpret_cast<uint64_t>(physical + delta + kPageSize)) ||
         RexGmFaultIn(reinterpret_cast<uint64_t>(physical + delta + kPageSize)))) {
      return fail("hole", i);
    }
    if ((i + 1) % 500 == 0) {
      std::fprintf(stderr, "[probe] %s committed %zu/%zu pages\n", layout, i + 1, kCount);
    }
  }
  if (RexGmCommittedBytes() != initial_committed + kCount * kPageSize ||
      RexGmMappedBytes() != initial_mapped + 2 * kCount * kPageSize) {
    return fail("accounting", kCount);
  }
  for (size_t i = 0; i < kCount; ++i) {
    const size_t delta = i * kStride;
    if (*reinterpret_cast<volatile uint64_t*>(physical + delta) != (kPattern | i)) {
      return fail("retained-data", i);
    }
  }
  // Watch one view without changing the other alias or the shadow data.
  RexGmAccess old = REX_GM_NONE;
  if (!RexGmProtect(physical, kPageSize, REX_GM_READ, &old) || old != REX_GM_WRITE) {
    return fail("protect", 0);
  }
  RexGmAccess access = REX_GM_NONE;
  size_t protected_length = 0;
  if (!RexGmQueryProtect(physical, &protected_length, &access) ||
      access != REX_GM_READ ||
      *reinterpret_cast<volatile uint64_t*>(mirror) != kPattern) {
    return fail("protection-alias", 0);
  }
  if (!RexGmProtect(physical, kPageSize, REX_GM_WRITE, &old) ||
      *reinterpret_cast<volatile uint64_t*>(physical) != kPattern) {
    return fail("unprotect", 0);
  }
  for (size_t i = 0; i < kCount; i += 3) {
    const size_t delta = i * kStride;
    if (!RexGmDecommit(kPhysicalOffset + delta, kPageSize) ||
        RexGmShadowFor(reinterpret_cast<uint64_t>(physical + delta)) ||
        RexGmFaultIn(reinterpret_cast<uint64_t>(mirror + delta))) {
      return fail("decommit", i);
    }
    if (!RexGmCommit(kPhysicalOffset + delta, kPageSize, REX_GM_WRITE) ||
        !RexGmFaultIn(reinterpret_cast<uint64_t>(physical + delta)) ||
        !RexGmFaultIn(reinterpret_cast<uint64_t>(mirror + delta))) {
      return fail("recommit", i);
    }
    if (*reinterpret_cast<volatile uint64_t*>(physical + delta) != 0 ||
        *reinterpret_cast<volatile uint64_t*>(mirror + delta) != 0) {
      return fail("zero-on-recommit", i);
    }
  }
  if (!RexGmDecommit(kPhysicalOffset, kSpan) ||
      RexGmCommittedBytes() != initial_committed ||
      RexGmMappedBytes() != initial_mapped) {
    return fail("release-accounting", kCount);
  }
  std::fprintf(stderr,
               "[probe] PASS %zu %s physical commits, two aliases, "
               "retained data, protection, decommit/recommit and accounting\n",
               kCount, layout);
  return true;
}

}  // namespace

int main() {
  if (!OpenProbeLog()) return 1;
  std::fprintf(stderr, "[probe] F.E.A.R. NX Horizon compatibility probe start\n");

  bool success = true;
  const Handle process_handle = static_cast<Handle>(RexGmProcessHandle());
  uint64_t current_pid = 0;
  uint64_t handle_pid = 0;
  Result result = svcGetProcessId(&current_pid, CUR_PROCESS_HANDLE);
  if (R_FAILED(result)) {
    LogResult("svcGetProcessId(CUR_PROCESS_HANDLE)", result);
    success = false;
  } else if (process_handle == INVALID_HANDLE || process_handle == CUR_PROCESS_HANDLE ||
             R_FAILED(result = svcGetProcessId(&handle_pid, process_handle)) ||
             current_pid != handle_pid) {
    if (R_FAILED(result)) LogResult("svcGetProcessId(acquired handle)", result);
    std::fprintf(stderr, "[probe] FAIL acquired process handle does not match current PID\n");
    success = false;
  } else {
    std::fprintf(stderr, "[probe] PASS real self process handle pid=%llu\n",
                 static_cast<unsigned long long>(current_pid));
  }

  uint8_t* base = RexGmInit(kProbeWindowSize);
  if (!base) {
    std::fprintf(stderr, "[probe] FAIL RexGmInit result=0x%08X\n",
                 unsigned(RexGmLastResult()));
    success = false;
  } else {
    std::fprintf(stderr, "[probe] reserved test window at %p size=0x%zX\n",
                 static_cast<void*>(base), kProbeWindowSize);
    if (!RunAliasProbe(base)) success = false;
    // 2600 sparse pages with two aliases exhausted Eden's finite block-metadata
    // pool at index 2500. Keep that limit result in the test report; use 2000
    // sparse and 2600 dense pages here to exercise semantics below that ceiling.
    if (success && !RunPhysicalProbe(base, "sparse", 2000, 3 * kPageSize,
                                    0x108000000ull, 0xA8000000ull))
      success = false;
    if (success && !RunPhysicalProbe(base, "dense", 2600, kPageSize,
                                    0x110000000ull, 0xB0000000ull))
      success = false;
  }

  // Install exercises the SDK's real 16-trial SIMD/FPCR/FPSR regression probe
  // before registering our actual data-abort handler. Any missed trap or
  // register mismatch aborts in the SDK and lands in the dedicated stderr log.
  HandlerState handler_state{};
  if (base) handler_state.fault_address = reinterpret_cast<uint64_t>(base + kFaultOffset);
  rex::arch::ExceptionHandler::Install(&InstallProbeExceptionHandler, &handler_state);
  std::fprintf(stderr,
               "[probe] PASS ExceptionHandler::Install returned after SDK vector/FP probe\n");

  if (base) {
    if (!RunCustomDataAbortProbe(handler_state)) success = false;
    RexGmShutdown();
    std::fprintf(stderr, "[probe] released test window\n");
  }

  rex::arch::ExceptionHandler::Uninstall(&InstallProbeExceptionHandler, &handler_state);
  std::fprintf(stderr, "[probe] RESULT %s\n", success ? "PASS" : "FAIL");
  std::fflush(stderr);
  if (success) {
    // libnx's direct orderly process exit bypasses the SDK's intentional
    // atexit crash hook, which treats a normal C exit as unexpected.
    svcExitProcess();
  }
  return 1;
}
