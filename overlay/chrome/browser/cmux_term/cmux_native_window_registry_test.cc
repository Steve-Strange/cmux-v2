// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-compilable tests for native-container routing policy (no Chromium,
// no gtest).

#include "chrome/browser/cmux_term/cmux_native_window_registry.h"

#include <cstdio>

using namespace cmux;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

BrowserProfileDomain Normal(uint64_t profile = 1) {
  return {profile, BrowsingMode::kNormal};
}

BrowserRouteRequest Request(BrowserCreationIntent intent,
                            BrowserProfileDomain domain = Normal()) {
  BrowserRouteRequest request;
  request.domain = domain;
  request.intent = intent;
  return request;
}

void TestContainerAndWorkspaceIdentity() {
  CmuxNativeWindowRegistry registry;
  NativeWindowId first = registry.RegisterWindow(Normal());
  NativeWindowId second = registry.RegisterWindow(Normal());
  Check(first != second, "native container ids are process-unique");

  WorkspaceLocator first_home{first, 1};
  WorkspaceLocator second_home{second, 1};
  Check(registry.RegisterWorkspace(first_home),
        "register first container local workspace 1");
  Check(registry.RegisterWorkspace(second_home),
        "same local workspace id is valid in another container");
  Check(registry.workspace_count() == 2,
        "composite workspace locator prevents collisions");
  Check(!registry.RegisterWorkspace(first_home),
        "duplicate locator is rejected");

  Check(registry.UnregisterWindow(first), "unregister first container");
  Check(!registry.ContainsWorkspace(first_home),
        "container teardown removes its workspace locators");
  Check(registry.ContainsWorkspace(second_home),
        "other container workspace survives teardown");
}

void TestDurableWindowGroupIdentity() {
  CmuxNativeWindowRegistry registry;
  const std::string primary = "11111111-1111-4111-8111-111111111111";
  const std::string secondary = "22222222-2222-4222-8222-222222222222";
  const NativeWindowId first = registry.RegisterWindow(Normal(), primary);
  const NativeWindowId second = registry.RegisterWindow(Normal(), secondary);
  Check(first != kInvalidNativeWindowId && second != kInvalidNativeWindowId,
        "distinct durable groups register in one profile");
  Check(registry.GroupForWindow(first) == primary &&
            registry.GroupForWindow(second) == secondary,
        "runtime windows retain their durable group IDs");
  Check(registry.RegisterWindow(Normal(), primary) == kInvalidNativeWindowId,
        "duplicate live group in one profile is rejected");
  Check(registry.RegisterWindow(Normal(2), primary) != kInvalidNativeWindowId,
        "same group text remains isolated by profile domain");
  Check(registry.UnregisterWindow(first) &&
            registry.RegisterWindow(Normal(), primary) !=
                kInvalidNativeWindowId,
        "closed durable group can be restored deliberately");
}

void TestStandardWindowAndWorkspaceRouting() {
  CmuxNativeWindowRegistry registry;
  NativeWindowId window = registry.RegisterWindow(Normal());

  BrowserRouteRequest new_window =
      Request(BrowserCreationIntent::kStandardNewWindow);
  new_window.invoking_window = window;
  BrowserRouteDecision decision = registry.Resolve(new_window);
  Check(decision.action == BrowserRouteAction::kCreateNativeWindow,
        "standard New Window always creates native container");

  BrowserRouteRequest new_workspace =
      Request(BrowserCreationIntent::kNewWorkspace);
  new_workspace.invoking_window = window;
  decision = registry.Resolve(new_workspace);
  Check(decision.action == BrowserRouteAction::kAdoptAsNewWorkspace &&
            decision.window == window,
        "cmux New Workspace adopts into invoking container");

  new_workspace.invoking_window.reset();
  decision = registry.Resolve(new_workspace);
  Check(decision.action == BrowserRouteAction::kCreateNativeWindow,
        "missing workspace invocation context does not guess a container");
}

void TestExtensionAdoptionPolicy() {
  CmuxNativeWindowRegistry registry;
  NativeWindowId normal = registry.RegisterWindow(Normal());
  BrowserProfileDomain incognito_domain{1, BrowsingMode::kIncognito};
  NativeWindowId incognito = registry.RegisterWindow(incognito_domain);

  BrowserRouteRequest extension =
      Request(BrowserCreationIntent::kExtensionWindowCreate);
  extension.invoking_window = normal;
  BrowserRouteDecision decision = registry.Resolve(extension);
  Check(decision.action == BrowserRouteAction::kAdoptAsNewWorkspace &&
            decision.window == normal,
        "extension normal Browser adopts to invoking compatible container");

  extension.invoking_window = incognito;
  decision = registry.Resolve(extension);
  Check(decision.action == BrowserRouteAction::kCreateNativeWindow,
        "normal Browser cannot enter incognito container");

  BrowserRouteRequest incognito_extension = Request(
      BrowserCreationIntent::kExtensionWindowCreate, incognito_domain);
  incognito_extension.invoking_window = incognito;
  decision = registry.Resolve(incognito_extension);
  Check(decision.action == BrowserRouteAction::kAdoptAsNewWorkspace &&
            decision.window == incognito,
        "incognito Browser may adopt within same privacy domain");
}

void TestExplicitWorkspaceAndFallbackTypes() {
  CmuxNativeWindowRegistry registry;
  NativeWindowId window = registry.RegisterWindow(Normal());
  WorkspaceLocator workspace{window, 7};
  Check(registry.RegisterWorkspace(workspace), "register assigned workspace");

  BrowserRouteRequest assigned = Request(BrowserCreationIntent::kUnknown);
  assigned.assigned_workspace = workspace;
  BrowserRouteDecision decision = registry.Resolve(assigned);
  Check(decision.action == BrowserRouteAction::kAttachToExistingWorkspace &&
            decision.workspace == workspace && decision.window == window,
        "valid assigned workspace routes exactly to its owner");

  assigned.assigned_workspace = WorkspaceLocator{window, 999};
  decision = registry.Resolve(assigned);
  Check(decision.action == BrowserRouteAction::kUseChromiumWindow,
        "stale workspace assignment fails closed");

  BrowserRouteRequest popup =
      Request(BrowserCreationIntent::kExtensionWindowCreate);
  popup.surface = BrowserSurfaceType::kPopup;
  popup.invoking_window = window;
  decision = registry.Resolve(popup);
  Check(decision.action == BrowserRouteAction::kUseChromiumWindow,
        "popup keeps Chromium BrowserView path");

  popup.surface = BrowserSurfaceType::kApp;
  decision = registry.Resolve(popup);
  Check(decision.action == BrowserRouteAction::kUseChromiumWindow,
        "app window keeps Chromium BrowserView path");
}

void TestActivationIsDomainScoped() {
  CmuxNativeWindowRegistry registry;
  NativeWindowId first = registry.RegisterWindow(Normal());
  NativeWindowId other_profile = registry.RegisterWindow(Normal(2));
  NativeWindowId second = registry.RegisterWindow(Normal());
  Check(registry.MostRecentlyActiveWindow(Normal()) == second,
        "newest compatible container starts most recent");
  Check(registry.ActivateWindow(first), "activate existing container");
  Check(registry.MostRecentlyActiveWindow(Normal()) == first,
        "activation updates compatible recency");
  Check(registry.MostRecentlyActiveWindow(Normal(2)) == other_profile,
        "profile domains maintain separate active candidates");
  Check(!registry.ActivateWindow(999), "cannot activate unknown container");
}

}  // namespace

int main() {
  std::printf("cmux_native_window_registry_test\n");
  TestContainerAndWorkspaceIdentity();
  TestDurableWindowGroupIdentity();
  TestStandardWindowAndWorkspaceRouting();
  TestExtensionAdoptionPolicy();
  TestExplicitWorkspaceAndFallbackTypes();
  TestActivationIsDomainScoped();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
