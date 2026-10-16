// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "chrome/browser/cmux_term/cmux_terminal_host_connection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace cmux {
namespace {

constexpr uint64_t kHelloRequestId = 1;
constexpr size_t kMaxHandshakePayload = 4096;
constexpr std::chrono::milliseconds kCancellationPollSlice(25);

TerminalHostConnectResult Failure(TerminalHostConnectError error,
                                  std::string detail = std::string()) {
  TerminalHostConnectResult result;
  result.error = error;
  result.message = TerminalHostConnectErrorMessage(error);
  if (!detail.empty()) {
    result.message.append(": ");
    result.message.append(detail);
  }
  return result;
}

bool IsCancelled(
    const std::shared_ptr<TerminalHostConnectCancellation>& cancellation) {
  return cancellation && cancellation->cancelled();
}

#if !defined(_WIN32)

using Deadline = std::chrono::steady_clock::time_point;

TerminalHostConnectError WaitForDescriptor(
    int descriptor,
    short events,
    Deadline deadline,
    const std::shared_ptr<TerminalHostConnectCancellation>& cancellation,
    std::string* detail) {
  for (;;) {
    if (IsCancelled(cancellation)) {
      return TerminalHostConnectError::kCancelled;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return TerminalHostConnectError::kTimedOut;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto wait = std::max(std::chrono::milliseconds(1),
                               std::min(remaining, kCancellationPollSlice));
    pollfd poll_descriptor = {descriptor, events, 0};
    const int result =
        poll(&poll_descriptor, 1, static_cast<int>(wait.count()));
    if (result == 0) {
      continue;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (detail) {
        *detail = std::strerror(errno);
      }
      return TerminalHostConnectError::kSocketFailed;
    }
    if ((poll_descriptor.revents & events) != 0) {
      return TerminalHostConnectError::kNone;
    }
    if (detail) {
      *detail = "socket closed during terminal-host handshake";
    }
    return TerminalHostConnectError::kConnectFailed;
  }
}

TerminalHostConnectError WriteExact(
    int descriptor,
    std::string_view bytes,
    Deadline deadline,
    const std::shared_ptr<TerminalHostConnectCancellation>& cancellation,
    std::string* detail) {
  while (!bytes.empty()) {
    if (IsCancelled(cancellation)) {
      return TerminalHostConnectError::kCancelled;
    }
#if defined(__linux__)
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t written =
        send(descriptor, bytes.data(), bytes.size(), kSendFlags);
    if (written > 0) {
      bytes.remove_prefix(static_cast<size_t>(written));
      continue;
    }
    if (written == 0) {
      if (detail) {
        *detail = "socket closed while writing ClientHello";
      }
      return TerminalHostConnectError::kWriteFailed;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      if (detail) {
        *detail = std::strerror(errno);
      }
      return TerminalHostConnectError::kWriteFailed;
    }
    const TerminalHostConnectError wait =
        WaitForDescriptor(descriptor, POLLOUT, deadline, cancellation, detail);
    if (wait != TerminalHostConnectError::kNone) {
      return wait;
    }
  }
  return TerminalHostConnectError::kNone;
}

TerminalHostConnectError ReadExact(
    int descriptor,
    size_t length,
    std::vector<uint8_t>* output,
    Deadline deadline,
    const std::shared_ptr<TerminalHostConnectCancellation>& cancellation,
    std::string* detail) {
  if (!output) {
    return TerminalHostConnectError::kReadFailed;
  }
  output->clear();
  output->reserve(length);
  std::array<uint8_t, kMaxHandshakePayload> chunk{};
  size_t offset = 0;
  while (offset < length) {
    if (IsCancelled(cancellation)) {
      return TerminalHostConnectError::kCancelled;
    }
    const size_t requested = std::min(length - offset, chunk.size());
    const ssize_t count = recv(descriptor, chunk.data(), requested, 0);
    if (count > 0) {
      const size_t received = static_cast<size_t>(count);
      output->insert(output->end(), chunk.begin(),
                     std::next(chunk.begin(), received));
      offset += received;
      continue;
    }
    if (count == 0) {
      if (detail) {
        *detail = "socket closed during terminal-host handshake";
      }
      return TerminalHostConnectError::kReadFailed;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      if (detail) {
        *detail = std::strerror(errno);
      }
      return TerminalHostConnectError::kReadFailed;
    }
    const TerminalHostConnectError wait =
        WaitForDescriptor(descriptor, POLLIN, deadline, cancellation, detail);
    if (wait != TerminalHostConnectError::kNone) {
      return wait;
    }
  }
  return TerminalHostConnectError::kNone;
}

TerminalHostConnectError ValidateEndpointFilesystem(
    const TerminalHostRendererGrant& grant,
    std::string* detail) {
  const std::string expected_parent =
      "/tmp/cmux-th-" + std::to_string(static_cast<uint64_t>(geteuid()));
  const size_t separator = grant.endpoint.rfind('/');
  if (separator == std::string::npos ||
      grant.endpoint.substr(0, separator) != expected_parent ||
      grant.endpoint.substr(separator + 1) !=
          EncodeTerminalHostId(grant.terminal_id) + ".sock") {
    if (detail) {
      *detail = "endpoint is outside the current user's terminal-host root";
    }
    return TerminalHostConnectError::kInvalidEndpointPath;
  }

  struct stat parent_status = {};
  if (lstat(expected_parent.c_str(), &parent_status) != 0) {
    if (detail) {
      *detail = std::strerror(errno);
    }
    return TerminalHostConnectError::kInvalidEndpointPath;
  }
  if (!S_ISDIR(parent_status.st_mode)) {
    if (detail) {
      *detail = "terminal-host parent is not a directory";
    }
    return TerminalHostConnectError::kInvalidEndpointType;
  }
  if (parent_status.st_uid != geteuid()) {
    return TerminalHostConnectError::kInvalidEndpointOwner;
  }
  if ((parent_status.st_mode & 0777) != 0700) {
    if (detail) {
      *detail = "terminal-host parent must have mode 0700";
    }
    return TerminalHostConnectError::kInvalidEndpointPermissions;
  }

  struct stat endpoint_status = {};
  if (lstat(grant.endpoint.c_str(), &endpoint_status) != 0) {
    if (detail) {
      *detail = std::strerror(errno);
    }
    return TerminalHostConnectError::kInvalidEndpointPath;
  }
  if (!S_ISSOCK(endpoint_status.st_mode)) {
    if (detail) {
      *detail = "terminal-host endpoint is not a socket";
    }
    return TerminalHostConnectError::kInvalidEndpointType;
  }
  if (endpoint_status.st_uid != geteuid()) {
    return TerminalHostConnectError::kInvalidEndpointOwner;
  }
  if ((endpoint_status.st_mode & 0777) != 0600) {
    if (detail) {
      *detail = "terminal-host endpoint must have mode 0600";
    }
    return TerminalHostConnectError::kInvalidEndpointPermissions;
  }
  return TerminalHostConnectError::kNone;
}

TerminalHostConnectError ValidatePeerCredentials(int descriptor,
                                                 std::string* detail) {
#if defined(__APPLE__)
  uid_t peer_uid = std::numeric_limits<uid_t>::max();
  gid_t peer_gid = std::numeric_limits<gid_t>::max();
  if (getpeereid(descriptor, &peer_uid, &peer_gid) != 0) {
    if (detail) {
      *detail = std::strerror(errno);
    }
    return TerminalHostConnectError::kPeerCredentialMismatch;
  }
  if (peer_uid != geteuid()) {
    if (detail) {
      *detail = "terminal-host peer belongs to another user";
    }
    return TerminalHostConnectError::kPeerCredentialMismatch;
  }
#elif defined(__linux__)
  struct ucred credentials = {};
  socklen_t length = sizeof(credentials);
  if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) !=
          0 ||
      length != sizeof(credentials) || credentials.uid != geteuid()) {
    if (detail) {
      *detail = "terminal-host peer credentials do not match current user";
    }
    return TerminalHostConnectError::kPeerCredentialMismatch;
  }
#else
  if (detail) {
    *detail = "peer credentials are unavailable on this POSIX platform";
  }
  return TerminalHostConnectError::kUnsupportedPlatform;
#endif
  return TerminalHostConnectError::kNone;
}

#endif  // !defined(_WIN32)

}  // namespace

TerminalHostConnectCancellation::TerminalHostConnectCancellation() = default;
TerminalHostConnectCancellation::~TerminalHostConnectCancellation() = default;

void TerminalHostConnectCancellation::Cancel() {
  cancelled_.store(true, std::memory_order_release);
}

bool TerminalHostConnectCancellation::cancelled() const {
  return cancelled_.load(std::memory_order_acquire);
}

TerminalHostAuthenticatedSocket::TerminalHostAuthenticatedSocket() = default;

TerminalHostAuthenticatedSocket::TerminalHostAuthenticatedSocket(
    int descriptor,
    TerminalHostId terminal_id,
    TerminalHostIncarnation incarnation,
    TerminalHostCapabilityRights rights,
    uint32_t protocol_flags)
    : descriptor_(descriptor),
      terminal_id_(terminal_id),
      incarnation_(incarnation),
      rights_(rights),
      protocol_flags_(protocol_flags) {}

TerminalHostAuthenticatedSocket::TerminalHostAuthenticatedSocket(
    TerminalHostAuthenticatedSocket&& other)
    : descriptor_(std::exchange(other.descriptor_, -1)),
      terminal_id_(other.terminal_id_),
      incarnation_(other.incarnation_),
      rights_(other.rights_),
      protocol_flags_(other.protocol_flags_) {}

TerminalHostAuthenticatedSocket& TerminalHostAuthenticatedSocket::operator=(
    TerminalHostAuthenticatedSocket&& other) {
  if (this != &other) {
    Reset();
    descriptor_ = std::exchange(other.descriptor_, -1);
    terminal_id_ = other.terminal_id_;
    incarnation_ = other.incarnation_;
    rights_ = other.rights_;
    protocol_flags_ = other.protocol_flags_;
  }
  return *this;
}

TerminalHostAuthenticatedSocket::~TerminalHostAuthenticatedSocket() {
  Reset();
}

int TerminalHostAuthenticatedSocket::TakeDescriptor() {
  return std::exchange(descriptor_, -1);
}

void TerminalHostAuthenticatedSocket::Reset() {
  if (descriptor_ < 0) {
    return;
  }
#if defined(_WIN32)
  closesocket(static_cast<SOCKET>(descriptor_));
#else
  close(descriptor_);
#endif
  descriptor_ = -1;
}

TerminalHostConnectResult::TerminalHostConnectResult() = default;
TerminalHostConnectResult::TerminalHostConnectResult(
    TerminalHostConnectResult&&) = default;
TerminalHostConnectResult& TerminalHostConnectResult::operator=(
    TerminalHostConnectResult&&) = default;
TerminalHostConnectResult::~TerminalHostConnectResult() = default;

TerminalHostConnectResult ConnectAndAuthenticateTerminalHost(
    const TerminalHostRendererGrant& grant,
    std::chrono::milliseconds timeout,
    const std::shared_ptr<TerminalHostConnectCancellation>& cancellation) {
#if defined(_WIN32)
  static_cast<void>(grant);
  static_cast<void>(timeout);
  if (IsCancelled(cancellation)) {
    return Failure(TerminalHostConnectError::kCancelled);
  }
  return Failure(TerminalHostConnectError::kUnsupportedPlatform,
                 "direct terminal hosts are POSIX-only");
#else
  if (IsCancelled(cancellation)) {
    return Failure(TerminalHostConnectError::kCancelled);
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return Failure(TerminalHostConnectError::kTimedOut);
  }
  const Deadline deadline = std::chrono::steady_clock::now() + timeout;
  std::string detail;
  TerminalHostConnectError status = ValidateEndpointFilesystem(grant, &detail);
  if (status != TerminalHostConnectError::kNone) {
    return Failure(status, std::move(detail));
  }

  const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return Failure(TerminalHostConnectError::kSocketFailed,
                   std::strerror(errno));
  }
  TerminalHostAuthenticatedSocket owned(descriptor, grant.terminal_id,
                                        grant.incarnation, grant.rights, 0);
  const int descriptor_flags = fcntl(descriptor, F_GETFD);
  const int status_flags = fcntl(descriptor, F_GETFL);
  if (descriptor_flags < 0 || status_flags < 0 ||
      fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
      fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) != 0) {
    return Failure(TerminalHostConnectError::kSocketFailed,
                   std::strerror(errno));
  }
#if defined(__APPLE__)
  int enabled = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 sizeof(enabled)) != 0) {
    return Failure(TerminalHostConnectError::kSocketFailed,
                   std::strerror(errno));
  }
#endif

  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  if (grant.endpoint.size() >= sizeof(address.sun_path)) {
    return Failure(TerminalHostConnectError::kInvalidEndpointPath,
                   "terminal-host endpoint is too long");
  }
  std::string terminated_endpoint = grant.endpoint;
  terminated_endpoint.push_back('\0');
  std::copy(terminated_endpoint.begin(), terminated_endpoint.end(),
            std::begin(address.sun_path));
  int connect_result;
  do {
    connect_result =
        connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                       grant.endpoint.size() + 1));
  } while (connect_result != 0 && errno == EINTR);
  if (connect_result != 0 && errno != EINPROGRESS && errno != EAGAIN) {
    return Failure(TerminalHostConnectError::kConnectFailed,
                   std::strerror(errno));
  }
  if (connect_result != 0) {
    status =
        WaitForDescriptor(descriptor, POLLOUT, deadline, cancellation, &detail);
    if (status != TerminalHostConnectError::kNone) {
      return Failure(status, std::move(detail));
    }
    int socket_error = 0;
    socklen_t socket_error_length = sizeof(socket_error);
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                   &socket_error_length) != 0 ||
        socket_error != 0) {
      return Failure(TerminalHostConnectError::kConnectFailed,
                     std::strerror(socket_error != 0 ? socket_error : errno));
    }
  }
  status = ValidatePeerCredentials(descriptor, &detail);
  if (status != TerminalHostConnectError::kNone) {
    return Failure(status, std::move(detail));
  }

  TerminalHostClientHello hello;
  hello.min_version = kTerminalHostProtocolVersion;
  hello.max_version = kTerminalHostProtocolVersion;
  hello.role = TerminalHostClientRole::kRenderer;
  hello.requested_rights = grant.rights;
  hello.terminal_id = grant.terminal_id;
  hello.token = grant.token;
  std::vector<uint8_t> hello_payload;
  if (EncodeTerminalHostClientHello(hello, &hello_payload) !=
      TerminalHostProtocolError::kNone) {
    return Failure(TerminalHostConnectError::kInvalidHostHello,
                   "could not encode ClientHello");
  }
  TerminalHostFrame hello_frame;
  hello_frame.kind = TerminalHostMessageKind::kClientHello;
  hello_frame.flags = kTerminalHostFlagViewerSizeAcks;
  hello_frame.request_id = kHelloRequestId;
  hello_frame.payload = std::move(hello_payload);
  std::vector<uint8_t> encoded_hello;
  if (EncodeTerminalHostFrame(hello_frame, &encoded_hello) !=
      TerminalHostProtocolError::kNone) {
    return Failure(TerminalHostConnectError::kInvalidHostHello,
                   "could not frame ClientHello");
  }
  status = WriteExact(
      descriptor,
      std::string_view(reinterpret_cast<const char*>(encoded_hello.data()),
                       encoded_hello.size()),
      deadline, cancellation, &detail);
  if (status != TerminalHostConnectError::kNone) {
    return Failure(status, std::move(detail));
  }

  std::vector<uint8_t> response_header;
  status = ReadExact(descriptor, kTerminalHostHeaderLength, &response_header,
                     deadline, cancellation, &detail);
  if (status != TerminalHostConnectError::kNone) {
    return Failure(status, std::move(detail));
  }
  TerminalHostFrameHeader parsed_header;
  if (DecodeTerminalHostFrameHeader(
          std::string_view(
              reinterpret_cast<const char*>(response_header.data()),
              response_header.size()),
          kMaxHandshakePayload,
          &parsed_header) != TerminalHostProtocolError::kNone ||
      parsed_header.version != kTerminalHostProtocolVersion ||
      parsed_header.kind != TerminalHostMessageKind::kHostHello ||
      (parsed_header.flags & ~kTerminalHostFlagViewerSizeAcks) != 0 ||
      parsed_header.request_id != kHelloRequestId ||
      parsed_header.sequence != 0) {
    return Failure(TerminalHostConnectError::kInvalidHostHello,
                   "invalid HostHello frame header");
  }
  std::vector<uint8_t> response_payload;
  status = ReadExact(descriptor, parsed_header.payload_length,
                     &response_payload, deadline, cancellation, &detail);
  if (status != TerminalHostConnectError::kNone) {
    return Failure(status, std::move(detail));
  }
  TerminalHostHostHello response;
  if (DecodeTerminalHostHostHello(
          std::string_view(
              reinterpret_cast<const char*>(response_payload.data()),
              response_payload.size()),
          &response) != TerminalHostProtocolError::kNone ||
      response.selected_version != kTerminalHostProtocolVersion) {
    return Failure(TerminalHostConnectError::kInvalidHostHello,
                   "invalid HostHello payload or selected version");
  }
  if (response.terminal_id != grant.terminal_id ||
      response.incarnation != grant.incarnation) {
    return Failure(TerminalHostConnectError::kIdentityMismatch);
  }
  if (response.granted_rights != grant.rights ||
      response.granted_rights != TerminalHostCapabilityRights::kRenderer) {
    return Failure(TerminalHostConnectError::kRightsMismatch);
  }
  if (IsCancelled(cancellation)) {
    return Failure(TerminalHostConnectError::kCancelled);
  }

  TerminalHostConnectResult result;
  result.socket = TerminalHostAuthenticatedSocket(
      owned.TakeDescriptor(), response.terminal_id, response.incarnation,
      response.granted_rights, parsed_header.flags);
  return result;
#endif
}

const char* TerminalHostConnectErrorMessage(TerminalHostConnectError error) {
  switch (error) {
    case TerminalHostConnectError::kNone:
      return "";
    case TerminalHostConnectError::kUnsupportedPlatform:
      return "terminal-host direct sockets are unsupported on this platform";
    case TerminalHostConnectError::kCancelled:
      return "terminal-host connection was cancelled";
    case TerminalHostConnectError::kTimedOut:
      return "terminal-host connection timed out";
    case TerminalHostConnectError::kInvalidEndpointPath:
      return "invalid terminal-host endpoint path";
    case TerminalHostConnectError::kInvalidEndpointType:
      return "invalid terminal-host endpoint type";
    case TerminalHostConnectError::kInvalidEndpointOwner:
      return "invalid terminal-host endpoint owner";
    case TerminalHostConnectError::kInvalidEndpointPermissions:
      return "insecure terminal-host endpoint permissions";
    case TerminalHostConnectError::kSocketFailed:
      return "terminal-host socket setup failed";
    case TerminalHostConnectError::kConnectFailed:
      return "terminal-host socket connect failed";
    case TerminalHostConnectError::kPeerCredentialMismatch:
      return "terminal-host peer credentials did not match";
    case TerminalHostConnectError::kWriteFailed:
      return "terminal-host ClientHello write failed";
    case TerminalHostConnectError::kReadFailed:
      return "terminal-host HostHello read failed";
    case TerminalHostConnectError::kInvalidHostHello:
      return "terminal-host returned an invalid HostHello";
    case TerminalHostConnectError::kIdentityMismatch:
      return "terminal-host identity changed during handshake";
    case TerminalHostConnectError::kRightsMismatch:
      return "terminal-host granted unexpected renderer rights";
  }
  return "unknown terminal-host connection error";
}

}  // namespace cmux
