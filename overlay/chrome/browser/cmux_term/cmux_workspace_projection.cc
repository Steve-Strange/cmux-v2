// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_workspace_projection.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace cmux {

namespace {

std::string StableToken(std::string_view domain, std::string_view value) {
  // Two independently salted FNV-1a lanes provide a stable opaque 128-bit
  // identifier without pulling Chromium into this host-testable model file.
  // These are identity tokens, not authentication primitives. Domain
  // separation keeps profile, group, and combined tokens independent even
  // when their input text happens to match.
  uint64_t high = UINT64_C(14695981039346656037);
  uint64_t low = UINT64_C(7809847782465536322);
  const auto mix = [&high, &low](std::string_view bytes) {
    for (const char character : bytes) {
      const uint8_t byte = static_cast<uint8_t>(character);
      high ^= byte;
      high *= UINT64_C(1099511628211);
      low ^= static_cast<uint8_t>(byte + 0x9d);
      low *= UINT64_C(14029467366897019727);
    }
  };
  mix("cmux-identity-v1");
  mix(domain);
  mix(std::string_view("\0", 1));
  mix(value);

  constexpr char kHex[] = "0123456789abcdef";
  std::string result(32, '0');
  for (size_t index = 16; index > 0; --index) {
    result[index - 1] = kHex[high & 0x0f];
    high >>= 4;
  }
  for (size_t index = result.size(); index > 16; --index) {
    result[index - 1] = kHex[low & 0x0f];
    low >>= 4;
  }
  return result;
}

bool ContainsKey(const std::set<std::string>& keys,
                 const std::string& key) {
  return !key.empty() && keys.find(key) != keys.end();
}

std::string UuidFromToken(std::string token) {
  token[12] = '5';
  token[16] = '8';
  return token.substr(0, 8) + "-" + token.substr(8, 4) + "-" +
         token.substr(12, 4) + "-" + token.substr(16, 4) + "-" +
         token.substr(20);
}

}  // namespace

CmuxFrontendIdentity::CmuxFrontendIdentity() = default;
CmuxFrontendIdentity::CmuxFrontendIdentity(const CmuxFrontendIdentity&) =
    default;
CmuxFrontendIdentity& CmuxFrontendIdentity::operator=(
    const CmuxFrontendIdentity&) = default;
CmuxFrontendIdentity::CmuxFrontendIdentity(CmuxFrontendIdentity&&) = default;
CmuxFrontendIdentity& CmuxFrontendIdentity::operator=(
    CmuxFrontendIdentity&&) = default;
CmuxFrontendIdentity::~CmuxFrontendIdentity() = default;

CmuxWorkspaceReconcilePlan::CmuxWorkspaceReconcilePlan() = default;
CmuxWorkspaceReconcilePlan::CmuxWorkspaceReconcilePlan(
    const CmuxWorkspaceReconcilePlan&) = default;
CmuxWorkspaceReconcilePlan& CmuxWorkspaceReconcilePlan::operator=(
    const CmuxWorkspaceReconcilePlan&) = default;
CmuxWorkspaceReconcilePlan::CmuxWorkspaceReconcilePlan(
    CmuxWorkspaceReconcilePlan&&) = default;
CmuxWorkspaceReconcilePlan& CmuxWorkspaceReconcilePlan::operator=(
    CmuxWorkspaceReconcilePlan&&) = default;
CmuxWorkspaceReconcilePlan::~CmuxWorkspaceReconcilePlan() = default;

CmuxPendingWorkspaceMutations::CmuxPendingWorkspaceMutations() = default;

CmuxPendingWorkspaceMutations::~CmuxPendingWorkspaceMutations() = default;

bool CmuxPendingWorkspaceMutations::RememberCreate(std::string key,
                                                   std::string mutation_id) {
  if (key.empty() || mutation_id.empty()) {
    return false;
  }
  return creates_.try_emplace(std::move(key), std::move(mutation_id)).second;
}

void CmuxPendingWorkspaceMutations::ForgetCreate(std::string_view key) {
  creates_.erase(std::string(key));
}

bool CmuxPendingWorkspaceMutations::HasCreate(std::string_view key) const {
  return creates_.find(std::string(key)) != creates_.end();
}

bool CmuxPendingWorkspaceMutations::RememberClose(std::string key,
                                                  std::string mutation_id) {
  if (key.empty() || mutation_id.empty()) {
    return false;
  }
  return closes_.try_emplace(std::move(key), std::move(mutation_id)).second;
}

void CmuxPendingWorkspaceMutations::ForgetClose(std::string_view key) {
  closes_.erase(std::string(key));
}

bool CmuxPendingWorkspaceMutations::HasClose(std::string_view key) const {
  return closes_.find(std::string(key)) != closes_.end();
}

bool CmuxPendingWorkspaceMutations::RememberRename(
    std::string key,
    std::string name,
    std::string mutation_id) {
  if (key.empty() || name.empty() || mutation_id.empty()) {
    return false;
  }
  const auto existing = renames_.find(key);
  if (existing != renames_.end() && existing->second.name == name) {
    return false;
  }
  renames_.insert_or_assign(
      std::move(key),
      CmuxPendingWorkspaceRename{std::move(name), std::move(mutation_id)});
  return true;
}

void CmuxPendingWorkspaceMutations::ForgetRename(std::string_view key) {
  renames_.erase(std::string(key));
}

bool CmuxPendingWorkspaceMutations::HasRename(std::string_view key) const {
  return renames_.find(std::string(key)) != renames_.end();
}

void CmuxPendingWorkspaceMutations::SetMove(std::string key,
                                            size_t desired_index,
                                            std::string mutation_id) {
  if (key.empty() || mutation_id.empty()) {
    return;
  }
  if (move_ && move_->key == key && move_->desired_index == desired_index) {
    return;
  }
  move_ = CmuxPendingWorkspaceMove{std::move(key), desired_index,
                                   std::move(mutation_id)};
}

void CmuxPendingWorkspaceMutations::ForgetMove() {
  move_.reset();
}

void CmuxPendingWorkspaceMutations::Clear() {
  creates_.clear();
  closes_.clear();
  renames_.clear();
  move_.reset();
}

std::set<std::string> CmuxPendingWorkspaceMutations::CreateKeys() const {
  std::set<std::string> keys;
  for (const auto& entry : creates_) {
    keys.insert(entry.first);
  }
  return keys;
}

std::set<std::string> CmuxPendingWorkspaceMutations::CloseKeys() const {
  std::set<std::string> keys;
  for (const auto& entry : closes_) {
    keys.insert(entry.first);
  }
  return keys;
}

CmuxWorkspaceProjection::CmuxWorkspaceProjection() = default;
CmuxWorkspaceProjection::CmuxWorkspaceProjection(
    const CmuxWorkspaceProjection&) = default;
CmuxWorkspaceProjection& CmuxWorkspaceProjection::operator=(
    const CmuxWorkspaceProjection&) = default;
CmuxWorkspaceProjection::CmuxWorkspaceProjection(CmuxWorkspaceProjection&&) =
    default;
CmuxWorkspaceProjection& CmuxWorkspaceProjection::operator=(
    CmuxWorkspaceProjection&&) = default;
CmuxWorkspaceProjection::~CmuxWorkspaceProjection() = default;

CmuxFrontendIdentity BuildCmuxFrontendIdentity(
    std::string_view profile_path,
    std::string_view browsing_mode,
    std::string_view window_group) {
  const std::string profile = StableToken("profile", profile_path);
  const std::string group = StableToken("window-group", window_group);
  const std::string mode =
      browsing_mode.empty() ? std::string("normal") : std::string(browsing_mode);
  const std::string scope =
      "profile/" + profile + "/" + mode + "/window/" + group;

  CmuxFrontendIdentity identity;
  identity.frontend = "cmux-browser";
  identity.scope = "window-group";
  identity.subject_key = scope;
  identity.origin = "cmux-browser:" + StableToken("origin", scope);
  identity.session = "cmux-browser-" + StableToken("session", scope);
  return identity;
}

bool IsCmuxUuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') {
        return false;
      }
      continue;
    }
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string BuildCmuxPrimaryWindowGroupId(std::string_view profile_path,
                                          std::string_view browsing_mode) {
  std::string source(profile_path);
  source.push_back('\0');
  source.append(browsing_mode);
  return UuidFromToken(StableToken("primary-window-group", source));
}

std::string CmuxDurableWorkspaceToken(
    const CmuxDurableWorkspaceAddress& address) {
  if (!IsCmuxUuid(address.window_group) ||
      !IsCmuxUuid(address.workspace_key)) {
    return std::string();
  }
  return "cmux-workspace:v2:" + address.window_group + ":" +
         address.workspace_key;
}

std::optional<CmuxDurableWorkspaceAddress> CmuxDurableWorkspaceFromToken(
    std::string_view token) {
  constexpr std::string_view kPrefix = "cmux-workspace:v2:";
  if (token.size() < kPrefix.size() ||
      token.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  token.remove_prefix(kPrefix.size());
  if (token.size() != 73 || token[36] != ':') {
    return std::nullopt;
  }
  CmuxDurableWorkspaceAddress address{std::string(token.substr(0, 36)),
                                      std::string(token.substr(37))};
  if (!IsCmuxUuid(address.window_group) ||
      !IsCmuxUuid(address.workspace_key)) {
    return std::nullopt;
  }
  return address;
}

CmuxWorkspaceReconcilePlan BuildCmuxWorkspaceReconcilePlan(
    const std::vector<CmuxCanonicalWorkspace>& canonical,
    const std::vector<CmuxLocalWorkspace>& local,
    const std::set<std::string>& pending_creates,
    const std::set<std::string>& pending_closes,
    const CmuxPendingWorkspaceMutations::Renames& pending_renames) {
  CmuxWorkspaceReconcilePlan plan;
  std::map<std::string, std::string> local_names;
  for (const CmuxLocalWorkspace& workspace : local) {
    if (!workspace.key.empty()) {
      local_names.emplace(workspace.key, workspace.name);
    }
  }

  std::set<std::string> canonical_keys;
  for (const CmuxCanonicalWorkspace& workspace : canonical) {
    if (workspace.key.empty() || !canonical_keys.insert(workspace.key).second) {
      continue;
    }
    plan.canonical_order.push_back(workspace.key);
    const auto local_workspace = local_names.find(workspace.key);
    if (local_workspace == local_names.end()) {
      // A locally requested close stays optimistically absent until cmux
      // either commits it or the operation is explicitly abandoned.
      if (!ContainsKey(pending_closes, workspace.key)) {
        plan.materialize.push_back(workspace);
      }
      continue;
    }
    const auto pending_rename = pending_renames.find(workspace.key);
    const bool local_matches_pending_rename =
        pending_rename != pending_renames.end() &&
        local_workspace->second == pending_rename->second.name;
    if (local_workspace->second != workspace.name &&
        !local_matches_pending_rename) {
      plan.rename.emplace_back(workspace.key, workspace.name);
    }
  }

  std::set<std::string> planned_removals;
  for (const CmuxLocalWorkspace& workspace : local) {
    if (workspace.key.empty() || ContainsKey(canonical_keys, workspace.key) ||
        ContainsKey(pending_creates, workspace.key)) {
      continue;
    }
    if (planned_removals.insert(workspace.key).second) {
      plan.remove.push_back(workspace.key);
    }
  }
  return plan;
}

std::vector<std::pair<std::string, size_t>> BuildCmuxWorkspaceOrderMoves(
    const std::vector<std::string>& current,
    const std::vector<std::string>& desired) {
  std::vector<std::pair<std::string, size_t>> moves;
  std::vector<std::string> simulated = current;
  std::set<std::string> seen;
  for (const std::string& key : desired) {
    if (key.empty() || !seen.insert(key).second) {
      continue;
    }
    const size_t desired_index = seen.size() - 1;
    const auto current_it =
        std::find(simulated.begin(), simulated.end(), key);
    const size_t current_index =
        static_cast<size_t>(current_it - simulated.begin());
    if (current_it != simulated.end() && current_index == desired_index) {
      continue;
    }
    moves.emplace_back(key, desired_index);
    if (current_it != simulated.end()) {
      simulated.erase(current_it);
    }
    simulated.insert(
        simulated.begin() + std::min(desired_index, simulated.size()), key);
  }
  return moves;
}

CmuxWorkspaceProjectionReloadBarrier::CmuxWorkspaceProjectionReloadBarrier(
    uint64_t generation)
    : generation_(generation) {}

void CmuxWorkspaceProjectionReloadBarrier::Reset(uint64_t generation) {
  generation_ = generation;
  max_observed_revision_ = 0;
  max_scheduled_revision_ = 0;
}

void CmuxWorkspaceProjectionReloadBarrier::Observe(uint64_t generation,
                                                    uint64_t revision) {
  if (generation != generation_) {
    return;
  }
  max_observed_revision_ = std::max(max_observed_revision_, revision);
}

bool CmuxWorkspaceProjectionReloadBarrier::TakeReload(
    uint64_t generation,
    uint64_t current_revision,
    bool blocked) {
  if (generation != generation_ || blocked ||
      max_observed_revision_ <= current_revision ||
      max_observed_revision_ <= max_scheduled_revision_) {
    return false;
  }
  max_scheduled_revision_ = max_observed_revision_;
  return true;
}

bool IsCurrentCmuxWorkspaceProjectionEvent(
    std::string_view snapshot_registry_id,
    std::string_view snapshot_generation,
    std::string_view event_registry_id,
    std::string_view event_generation) {
  return !snapshot_registry_id.empty() && !snapshot_generation.empty() &&
         snapshot_registry_id == event_registry_id &&
         snapshot_generation == event_generation;
}

bool IsNextCmuxWorkspaceRevision(std::string_view snapshot_registry_id,
                                 std::string_view snapshot_generation,
                                 uint64_t snapshot_revision,
                                 std::string_view event_registry_id,
                                 std::string_view event_generation,
                                 uint64_t event_revision) {
  return !snapshot_registry_id.empty() && !snapshot_generation.empty() &&
         snapshot_registry_id == event_registry_id &&
         snapshot_generation == event_generation &&
         snapshot_revision != UINT64_MAX &&
         event_revision == snapshot_revision + 1;
}

CmuxWorkspaceProjection NormalizeCmuxWorkspaceProjection(
    const std::vector<CmuxCanonicalWorkspace>& canonical,
    const CmuxWorkspaceProjection& candidate) {
  std::vector<std::string> canonical_order;
  std::set<std::string> canonical_keys;
  for (const CmuxCanonicalWorkspace& workspace : canonical) {
    if (!workspace.key.empty() && canonical_keys.insert(workspace.key).second) {
      canonical_order.push_back(workspace.key);
    }
  }

  std::map<std::string, CmuxWorkspaceProjectionEntry> candidate_by_key;
  for (const CmuxWorkspaceProjectionEntry& entry : candidate.workspaces) {
    if (ContainsKey(canonical_keys, entry.key)) {
      candidate_by_key.emplace(entry.key, entry);
    }
  }

  CmuxWorkspaceProjection normalized;
  normalized.selected_key =
      ContainsKey(canonical_keys, candidate.selected_key)
          ? candidate.selected_key
          : (canonical_order.empty() ? std::string() : canonical_order.front());
  normalized.workspaces.reserve(canonical_order.size());
  for (const std::string& key : canonical_order) {
    CmuxWorkspaceProjectionEntry entry;
    entry.key = key;
    if (const auto found = candidate_by_key.find(key);
        found != candidate_by_key.end()) {
      entry = found->second;
      entry.key = key;
    }
    if (!ContainsKey(canonical_keys, entry.parent_key) ||
        entry.parent_key == key) {
      entry.parent_key.clear();
    }
    if (entry.color < 0 || entry.color > 8) {
      entry.color = 0;
    }
    normalized.workspaces.push_back(std::move(entry));
  }

  std::map<std::string, size_t> normalized_index;
  for (size_t index = 0; index < normalized.workspaces.size(); ++index) {
    normalized_index.emplace(normalized.workspaces[index].key, index);
  }
  // Break cycles at the first canonical entry that observes each cycle.
  for (CmuxWorkspaceProjectionEntry& entry : normalized.workspaces) {
    std::set<std::string> path{entry.key};
    std::string parent = entry.parent_key;
    while (!parent.empty()) {
      if (!path.insert(parent).second) {
        entry.parent_key.clear();
        break;
      }
      const auto found = normalized_index.find(parent);
      if (found == normalized_index.end()) {
        entry.parent_key.clear();
        break;
      }
      parent = normalized.workspaces[found->second].parent_key;
    }
  }

  // Schema-v1 projections allowed arbitrary parent chains. The current model
  // supports one shallow group level, so migrate every surviving chain to its
  // ultimate representative before controller restore. This prevents applying
  // A->B followed by B->C from orphaning A in a singleton group.
  for (CmuxWorkspaceProjectionEntry& entry : normalized.workspaces) {
    std::string representative = entry.parent_key;
    while (!representative.empty()) {
      const auto found = normalized_index.find(representative);
      if (found == normalized_index.end()) {
        representative.clear();
        break;
      }
      const std::string& parent =
          normalized.workspaces[found->second].parent_key;
      if (parent.empty()) {
        break;
      }
      representative = parent;
    }
    entry.parent_key = std::move(representative);
  }
  return normalized;
}

}  // namespace cmux
