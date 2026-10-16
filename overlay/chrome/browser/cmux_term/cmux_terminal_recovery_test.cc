// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_terminal_recovery.h"

#include <cstdlib>
#include <iostream>

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

cmux::TerminalHostId Uuid(uint8_t seed) {
  cmux::TerminalHostId id{};
  for (size_t index = 0; index < id.size(); ++index) {
    id[index] = static_cast<uint8_t>(seed + index);
  }
  id[6] = static_cast<uint8_t>((id[6] & 0x0f) | 0x40);
  id[8] = static_cast<uint8_t>((id[8] & 0x3f) | 0x80);
  return id;
}

cmux::CmuxTerminalBinding Binding(uint64_t surface,
                                  uint8_t id_seed,
                                  uint8_t incarnation_seed) {
  cmux::CmuxTerminalBinding binding;
  binding.surface = surface;
  binding.terminal_id = Uuid(id_seed);
  binding.terminal_incarnation = Uuid(incarnation_seed);
  return binding;
}

void TestCreateThenUse() {
  cmux::CmuxTerminalRecovery recovery;
  Check(recovery.ActionForGeneration(0) ==
            cmux::CmuxTerminalRecoveryAction::kNone,
        "no action before a server generation exists");
  Check(recovery.ActionForGeneration(1) ==
            cmux::CmuxTerminalRecoveryAction::kCreate,
        "a terminal without identity may be created");
  const cmux::TerminalHostId reserved_id = Uuid(1);
  Check(recovery.ReserveTerminalId(reserved_id),
        "client reserves a terminal id before create");
  Check(recovery.ActionForGeneration(1) ==
            cmux::CmuxTerminalRecoveryAction::kCreate,
        "reservation alone does not imply the create reached the server");
  recovery.MarkCreateRequestSent();
  Check(recovery.ActionForGeneration(1) ==
            cmux::CmuxTerminalRecoveryAction::kResolve,
        "ambiguous create result resolves the reserved id without respawn");
  const cmux::CmuxTerminalBinding created = Binding(17, 1, 33);
  Check(recovery.AcceptCreated(1, created), "accept first create result");
  Check(recovery.has_stable_identity(), "create records durable identity");
  Check(recovery.ActionForGeneration(1) ==
            cmux::CmuxTerminalRecoveryAction::kUseSurface,
        "created surface is usable in its daemon generation");
  Check(recovery.SurfaceForGeneration(1) == 17,
        "surface is exposed only in matching generation");
  Check(!recovery.SurfaceForGeneration(2),
        "old daemon surface is not exposed in a new generation");
}

void TestReservedIdentityMustBeEchoed() {
  cmux::CmuxTerminalRecovery recovery;
  Check(recovery.ReserveTerminalId(Uuid(6)), "reserve expected identity");
  recovery.MarkCreateRequestSent();
  Check(!recovery.AcceptCreated(1, Binding(1, 7, 38)),
        "create response cannot substitute another terminal id");
  Check(recovery.terminal_error(),
        "substituted terminal id permanently fails closed");
}

void TestResolveCompletesAmbiguousCreate() {
  cmux::CmuxTerminalRecovery recovery;
  Check(recovery.ReserveTerminalId(Uuid(8)),
        "reserve identity for ambiguous create");
  recovery.MarkCreateRequestSent();
  const cmux::CmuxTerminalBinding resolved = Binding(22, 8, 40);
  Check(recovery.AcceptResolved(2, resolved),
        "resolve may be the first response that supplies incarnation");
  Check(recovery.binding() && *recovery.binding() == resolved,
        "resolved binding becomes the canonical local lease");
  Check(recovery.ActionForGeneration(2) ==
            cmux::CmuxTerminalRecoveryAction::kUseSurface,
        "resolved ambiguous create is immediately usable");
}

void TestSeededCanonicalPlacementNeverCreates() {
  cmux::CmuxTerminalRecovery adopting;
  Check(adopting.SeedKnownTerminal(
            Uuid(9), Uuid(41), std::nullopt, 0,
            cmux::CmuxTerminalLifecycle::kAdopting),
        "seed canonical placement without a runtime lease");
  Check(adopting.ActionForGeneration(7) ==
            cmux::CmuxTerminalRecoveryAction::kResolve,
        "seeded placement resolves instead of spawning a replacement");
  cmux::CmuxTerminalBinding adopted = Binding(70, 9, 41);
  Check(adopting.AcceptResolved(7, adopted),
        "seeded incarnation accepts its matching runtime lease");
  Check(adopting.ActionForGeneration(7) ==
            cmux::CmuxTerminalRecoveryAction::kUseSurface,
        "matching adopted runtime lease becomes usable");

  cmux::CmuxTerminalRecovery running;
  Check(running.SeedKnownTerminal(
            Uuid(10), Uuid(42), 71, 8,
            cmux::CmuxTerminalLifecycle::kRunning),
        "seed running canonical placement with a current lease");
  Check(running.SurfaceForGeneration(8) == 71,
        "seeded current-generation lease is immediately usable");
  Check(running.ActionForGeneration(9) ==
            cmux::CmuxTerminalRecoveryAction::kResolve,
        "daemon generation change still resolves the seeded identity");

  cmux::CmuxTerminalRecovery exited;
  Check(exited.SeedKnownTerminal(
            Uuid(11), Uuid(43), std::nullopt, 0,
            cmux::CmuxTerminalLifecycle::kExited),
        "seed exited canonical placement");
  Check(exited.exited() &&
            exited.ActionForGeneration(10) ==
                cmux::CmuxTerminalRecoveryAction::kNone,
        "seeded exit is retained and never respawned");
}

void TestCanonicalLifecycleReconcilesInPlace() {
  cmux::CmuxTerminalRecovery pending;
  Check(pending.SeedKnownTerminal(
            Uuid(12), std::nullopt, std::nullopt, 0,
            cmux::CmuxTerminalLifecycle::kLaunching),
        "seed pending launch without an incarnation");
  Check(pending.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kAdopting, std::nullopt),
        "pending launch advances to adopting in place");
  Check(pending.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kRunning, Uuid(44)),
        "pending placement learns its immutable running incarnation");
  Check(pending.stable_incarnation() == Uuid(44) &&
            pending.ActionForGeneration(12) ==
                cmux::CmuxTerminalRecoveryAction::kResolve,
        "pending-to-running still resolves and can never create");
  Check(!pending.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kRunning, Uuid(45)),
        "running placement rejects an incarnation replacement");
  Check(pending.terminal_error(),
        "incarnation replacement fails recovery closed");

  cmux::CmuxTerminalRecovery running;
  Check(running.SeedKnownTerminal(
            Uuid(13), Uuid(46), 73, 12,
            cmux::CmuxTerminalLifecycle::kRunning),
        "seed running placement before canonical exit");
  Check(running.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kAdopting, Uuid(46)),
        "running host may re-enter adoption when only admin transport drops");
  Check(running.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kRunning, Uuid(46)),
        "same host incarnation returns to running after admin reconnect");
  Check(running.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kExited, Uuid(46)),
        "running placement advances to exited");
  Check(running.exited() && !running.SurfaceForGeneration(12) &&
            running.FinalSurfaceForGeneration(12) == 73 &&
            running.ActionForGeneration(13) ==
                cmux::CmuxTerminalRecoveryAction::kNone,
        "canonical exit retains only a same-generation read-only lease");
  Check(!running.FinalSurfaceForGeneration(13),
        "an exited surface is never reused across daemon generations");
  Check(!running.ApplyCanonicalLifecycle(
            cmux::CmuxTerminalLifecycle::kRunning, Uuid(46)),
        "exited placement cannot be revived by a stale running event");
}

void TestDaemonRestartNeverCreatesReplacement() {
  cmux::CmuxTerminalRecovery recovery;
  const cmux::CmuxTerminalBinding created = Binding(17, 2, 34);
  Check(recovery.AcceptCreated(10, created), "accept initial durable binding");

  recovery.InvalidateLocalSurface();
  Check(recovery.has_stable_identity(),
        "disconnect retains terminal id and incarnation");
  Check(recovery.ActionForGeneration(11) ==
            cmux::CmuxTerminalRecoveryAction::kResolve,
        "new daemon generation resolves instead of creates");

  // terminal_not_found during adoption is represented by taking no state
  // transition and retrying. The action must remain resolve indefinitely.
  Check(recovery.ActionForGeneration(11) ==
            cmux::CmuxTerminalRecoveryAction::kResolve,
        "transient terminal_not_found cannot fall back to create");

  cmux::CmuxTerminalBinding rebound = created;
  rebound.surface = 91;
  Check(recovery.AcceptResolved(11, rebound),
        "same durable process may receive a new local surface id");
  Check(recovery.SurfaceForGeneration(11) == 91,
        "resolved surface replaces stale routing number");
  Check(recovery.ActionForGeneration(11) ==
            cmux::CmuxTerminalRecoveryAction::kUseSurface,
        "resolved surface becomes usable");
}

void TestIdentityMismatchFailsClosed() {
  cmux::CmuxTerminalRecovery recovery;
  const cmux::CmuxTerminalBinding created = Binding(7, 3, 35);
  Check(recovery.AcceptCreated(1, created), "accept binding before mismatch");
  recovery.InvalidateLocalSurface();

  cmux::CmuxTerminalBinding replacement = created;
  replacement.surface = 8;
  replacement.terminal_incarnation = Uuid(99);
  Check(!recovery.AcceptResolved(2, replacement),
        "changed incarnation is rejected");
  Check(recovery.terminal_error(), "identity mismatch is terminal error");
  Check(recovery.ActionForGeneration(2) ==
            cmux::CmuxTerminalRecoveryAction::kNone,
        "identity mismatch cannot create or resolve another shell");
  Check(!recovery.SurfaceForGeneration(2),
        "mismatched surface is never exposed");
}

void TestSecondCreateAndInvalidIdentityFailClosed() {
  cmux::CmuxTerminalRecovery duplicate;
  Check(duplicate.AcceptCreated(1, Binding(1, 4, 36)),
        "accept first create");
  Check(!duplicate.AcceptCreated(2, Binding(2, 4, 36)),
        "second create is forbidden after stable identity assignment");
  Check(duplicate.terminal_error(), "second create poisons recovery state");

  cmux::CmuxTerminalRecovery invalid;
  cmux::CmuxTerminalBinding binding;
  binding.surface = 4;
  Check(!invalid.AcceptCreated(1, binding), "zero UUIDs are rejected");
  Check(invalid.terminal_error(), "invalid create response fails closed");
}

void TestExitDoesNotForgetStableIdentity() {
  cmux::CmuxTerminalRecovery recovery;
  Check(recovery.AcceptCreated(3, Binding(44, 5, 37)),
        "accept binding before exit");
  recovery.MarkExited();
  Check(recovery.exited(), "exit is retained as terminal state");
  Check(recovery.has_stable_identity(), "exit retains durable identity");
  Check(recovery.ActionForGeneration(4) ==
            cmux::CmuxTerminalRecoveryAction::kNone,
        "exit never launches a replacement shell");
}

}  // namespace

int main() {
  TestCreateThenUse();
  TestReservedIdentityMustBeEchoed();
  TestResolveCompletesAmbiguousCreate();
  TestSeededCanonicalPlacementNeverCreates();
  TestCanonicalLifecycleReconcilesInPlace();
  TestDaemonRestartNeverCreatesReplacement();
  TestIdentityMismatchFailsClosed();
  TestSecondCreateAndInvalidIdentityFailClosed();
  TestExitDoesNotForgetStableIdentity();
  std::cout << "cmux-terminal-recovery: " << checks << " checks, " << failures
            << " failures\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
