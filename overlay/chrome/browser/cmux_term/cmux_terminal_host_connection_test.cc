// Copyright 2026 Manaflow, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later

// Host-compilable integration tests for the authenticated terminal-host
// socket broker. No Chromium or gtest dependencies.

#include "chrome/browser/cmux_term/cmux_terminal_host_connection.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}

#if !defined(_WIN32)

enum class ResponsePlan {
  kValidWithSnapshot,
  kLegacyWithSnapshot,
  kSelectedVersion,
  kHeaderVersion,
  kHeaderKind,
  kHeaderFlags,
  kHeaderRequest,
  kHeaderSequence,
  kReservedPayload,
  kTerminalIdentity,
  kIncarnation,
  kRights,
  kOversizePayload,
  kTruncatedHeader,
  kStall,
};

struct TestListener {
  TestListener() = default;
  TestListener(const TestListener&) = delete;
  TestListener& operator=(const TestListener&) = delete;
  TestListener(TestListener&& other)
      : descriptor(other.descriptor), endpoint(std::move(other.endpoint)) {
    other.descriptor = -1;
  }
  TestListener& operator=(TestListener&& other) {
    if (this != &other) {
      Reset();
      descriptor = other.descriptor;
      endpoint = std::move(other.endpoint);
      other.descriptor = -1;
    }
    return *this;
  }
  ~TestListener() { Reset(); }

  void Reset() {
    if (descriptor >= 0) {
      close(descriptor);
      descriptor = -1;
    }
    if (!endpoint.empty()) {
      unlink(endpoint.c_str());
    }
  }

  int descriptor = -1;
  std::string endpoint;
};

cmux::TerminalHostId TestUuid(uint32_t seed) {
  cmux::TerminalHostId id{};
  for (size_t index = 0; index < id.size(); ++index) {
    id[index] = static_cast<uint8_t>((seed * 37 + index * 19) & 0xff);
  }
  id[6] = static_cast<uint8_t>((id[6] & 0x0f) | 0x40);
  id[8] = static_cast<uint8_t>((id[8] & 0x3f) | 0x80);
  return id;
}

cmux::TerminalHostRendererGrant TestGrant(uint32_t sequence) {
  cmux::TerminalHostRendererGrant grant;
  grant.terminal_id =
      TestUuid(static_cast<uint32_t>(getpid()) ^ (sequence * 0x9e3779b9U));
  grant.incarnation = TestUuid(sequence ^ 0xa5a5f00dU);
  for (size_t index = 0; index < grant.token.size(); ++index) {
    grant.token[index] =
        static_cast<uint8_t>((sequence + index * 11 + 1) & 0xff);
  }
  grant.rights = cmux::TerminalHostCapabilityRights::kRenderer;
  grant.ttl_ms = 5000;
  grant.endpoint = "/tmp/cmux-th-" +
                   std::to_string(static_cast<uint64_t>(geteuid())) + "/" +
                   cmux::EncodeTerminalHostId(grant.terminal_id) + ".sock";
  return grant;
}

bool PrepareHostRoot() {
  const std::string root =
      "/tmp/cmux-th-" + std::to_string(static_cast<uint64_t>(geteuid()));
  if (mkdir(root.c_str(), 0700) != 0 && errno != EEXIST) {
    return false;
  }
  struct stat status = {};
  return lstat(root.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
         status.st_uid == geteuid() && (status.st_mode & 0777) == 0700;
}

TestListener Listen(const cmux::TerminalHostRendererGrant& grant,
                    mode_t mode = 0600) {
  TestListener listener;
  listener.endpoint = grant.endpoint;
  unlink(listener.endpoint.c_str());
  listener.descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener.descriptor < 0) {
    return listener;
  }
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  std::copy(listener.endpoint.begin(), listener.endpoint.end(),
            address.sun_path);
  address.sun_path[listener.endpoint.size()] = '\0';
  if (bind(listener.descriptor, reinterpret_cast<sockaddr*>(&address),
           static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                  listener.endpoint.size() + 1)) != 0 ||
      chmod(listener.endpoint.c_str(), mode) != 0 ||
      listen(listener.descriptor, 4) != 0) {
    listener.Reset();
  }
  return listener;
}

bool ReadExactBlocking(int descriptor, uint8_t* output, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const ssize_t count = recv(descriptor, output + offset, length - offset, 0);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool WriteAll(int descriptor, const std::vector<uint8_t>& bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
#if defined(__linux__)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    const ssize_t count =
        send(descriptor, bytes.data() + offset, bytes.size() - offset, kFlags);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

std::optional<cmux::TerminalHostFrame> ReadFrameWithTimeout(int descriptor) {
  cmux::TerminalHostFrameDecoder decoder;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd poll_descriptor = {descriptor, POLLIN, 0};
    if (poll(&poll_descriptor, 1, 25) < 0 && errno != EINTR) {
      return std::nullopt;
    }
    if ((poll_descriptor.revents & POLLIN) == 0) {
      continue;
    }
    uint8_t bytes[256];
    const ssize_t count = recv(descriptor, bytes, sizeof(bytes), 0);
    if (count <= 0) {
      return std::nullopt;
    }
    std::vector<cmux::TerminalHostFrame> frames;
    if (decoder.Push(std::string_view(reinterpret_cast<const char*>(bytes),
                                      static_cast<size_t>(count)),
                     &frames) != cmux::TerminalHostProtocolError::kNone) {
      return std::nullopt;
    }
    if (!frames.empty()) {
      return std::move(frames.front());
    }
  }
  return std::nullopt;
}

void Serve(int listener,
           cmux::TerminalHostRendererGrant grant,
           ResponsePlan plan,
           std::atomic_bool* observed_valid_hello) {
  pollfd poll_descriptor = {listener, POLLIN, 0};
  if (poll(&poll_descriptor, 1, 3000) <= 0) {
    return;
  }
  const int client = accept(listener, nullptr, nullptr);
  if (client < 0) {
    return;
  }
  std::array<uint8_t, cmux::kTerminalHostHeaderLength> header{};
  if (!ReadExactBlocking(client, header.data(), header.size())) {
    close(client);
    return;
  }
  cmux::TerminalHostFrameHeader parsed;
  if (cmux::DecodeTerminalHostFrameHeader(
          std::string_view(reinterpret_cast<const char*>(header.data()),
                           header.size()),
          4096, &parsed) != cmux::TerminalHostProtocolError::kNone) {
    close(client);
    return;
  }
  std::vector<uint8_t> payload(parsed.payload_length);
  if (!ReadExactBlocking(client, payload.data(), payload.size())) {
    close(client);
    return;
  }
  cmux::TerminalHostClientHello hello;
  const bool valid_hello =
      parsed.version == cmux::kTerminalHostProtocolVersion &&
      parsed.kind == cmux::TerminalHostMessageKind::kClientHello &&
      parsed.flags == cmux::kTerminalHostFlagViewerSizeAcks &&
      parsed.request_id == 1 && parsed.sequence == 0 &&
      cmux::DecodeTerminalHostClientHello(
          std::string_view(reinterpret_cast<const char*>(payload.data()),
                           payload.size()),
          &hello) == cmux::TerminalHostProtocolError::kNone &&
      hello.min_version == cmux::kTerminalHostProtocolVersion &&
      hello.max_version == cmux::kTerminalHostProtocolVersion &&
      hello.role == cmux::TerminalHostClientRole::kRenderer &&
      hello.requested_rights == grant.rights &&
      hello.terminal_id == grant.terminal_id && hello.token == grant.token;
  observed_valid_hello->store(valid_hello, std::memory_order_release);
  if (plan == ResponsePlan::kStall) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    close(client);
    return;
  }

  cmux::TerminalHostHostHello response;
  response.selected_version = plan == ResponsePlan::kSelectedVersion
                                  ? 2
                                  : cmux::kTerminalHostProtocolVersion;
  response.granted_rights = plan == ResponsePlan::kRights
                                ? cmux::TerminalHostCapabilityRights::kRead
                                : grant.rights;
  response.terminal_id = plan == ResponsePlan::kTerminalIdentity
                             ? TestUuid(0xdeadbeef)
                             : grant.terminal_id;
  response.incarnation = plan == ResponsePlan::kIncarnation
                             ? TestUuid(0x12345678)
                             : grant.incarnation;
  std::vector<uint8_t> response_payload;
  cmux::EncodeTerminalHostHostHello(response, &response_payload);
  if (plan == ResponsePlan::kReservedPayload) {
    response_payload[2] = 1;
  }
  cmux::TerminalHostFrame response_frame;
  response_frame.version = plan == ResponsePlan::kHeaderVersion ? 2 : 1;
  response_frame.kind = plan == ResponsePlan::kHeaderKind
                            ? cmux::TerminalHostMessageKind::kOutput
                            : cmux::TerminalHostMessageKind::kHostHello;
  response_frame.flags = plan == ResponsePlan::kHeaderFlags
                             ? cmux::kTerminalHostFlagColorsFollow
                             : (plan == ResponsePlan::kLegacyWithSnapshot
                                    ? 0
                                    : cmux::kTerminalHostFlagViewerSizeAcks);
  response_frame.request_id = plan == ResponsePlan::kHeaderRequest ? 2 : 1;
  response_frame.sequence = plan == ResponsePlan::kHeaderSequence ? 1 : 0;
  response_frame.payload = std::move(response_payload);
  std::vector<uint8_t> response_bytes;
  cmux::EncodeTerminalHostFrame(response_frame, &response_bytes);
  if (plan == ResponsePlan::kOversizePayload) {
    response_bytes.resize(cmux::kTerminalHostHeaderLength);
    const uint32_t length = 4097;
    response_bytes[12] = static_cast<uint8_t>(length);
    response_bytes[13] = static_cast<uint8_t>(length >> 8);
    response_bytes[14] = static_cast<uint8_t>(length >> 16);
    response_bytes[15] = static_cast<uint8_t>(length >> 24);
  } else if (plan == ResponsePlan::kTruncatedHeader) {
    response_bytes.resize(10);
  } else if (plan == ResponsePlan::kValidWithSnapshot ||
             plan == ResponsePlan::kLegacyWithSnapshot) {
    cmux::TerminalHostSnapshot snapshot;
    snapshot.cols = 123;
    snapshot.rows = 45;
    snapshot.replay = {'s', 'n', 'a', 'p'};
    std::vector<uint8_t> snapshot_payload;
    cmux::EncodeTerminalHostSnapshot(snapshot, &snapshot_payload);
    cmux::TerminalHostFrame snapshot_frame;
    snapshot_frame.kind = cmux::TerminalHostMessageKind::kSnapshot;
    snapshot_frame.sequence = 9;
    snapshot_frame.payload = std::move(snapshot_payload);
    std::vector<uint8_t> snapshot_bytes;
    cmux::EncodeTerminalHostFrame(snapshot_frame, &snapshot_bytes);
    response_bytes.insert(response_bytes.end(), snapshot_bytes.begin(),
                          snapshot_bytes.end());
  }
  WriteAll(client, response_bytes);
  close(client);
}

cmux::TerminalHostConnectResult RunCase(ResponsePlan plan,
                                        cmux::TerminalHostConnectError expected,
                                        uint32_t sequence) {
  cmux::TerminalHostRendererGrant grant = TestGrant(sequence);
  TestListener listener = Listen(grant);
  Check(listener.descriptor >= 0, "test terminal-host listener starts");
  std::atomic_bool observed(false);
  std::thread server(Serve, listener.descriptor, grant, plan, &observed);
  const std::shared_ptr<cmux::TerminalHostConnectCancellation> cancellation =
      std::make_shared<cmux::TerminalHostConnectCancellation>();
  cmux::TerminalHostConnectResult result =
      cmux::ConnectAndAuthenticateTerminalHost(
          grant,
          plan == ResponsePlan::kStall ? std::chrono::milliseconds(50)
                                       : std::chrono::seconds(2),
          cancellation);
  server.join();
  Check(observed.load(std::memory_order_acquire),
        "broker sends an exact renderer ClientHello");
  Check(result.error == expected, "broker reports expected handshake result");
  return result;
}

void TestSuccessfulHandshakeAndSnapshotBoundary() {
  cmux::TerminalHostConnectResult result =
      RunCase(ResponsePlan::kValidWithSnapshot,
              cmux::TerminalHostConnectError::kNone, 1);
  Check(result.socket && result.socket->valid(),
        "successful broker returns an owned descriptor");
  if (!result.socket) {
    return;
  }
  const int descriptor = result.socket->descriptor();
  Check((fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0,
        "authenticated descriptor remains CLOEXEC");
  Check((fcntl(descriptor, F_GETFL) & O_NONBLOCK) != 0,
        "authenticated descriptor remains nonblocking");
  Check(
      result.socket->rights() == cmux::TerminalHostCapabilityRights::kRenderer,
      "authenticated descriptor carries renderer rights");
  Check(result.socket->protocol_flags() ==
            cmux::kTerminalHostFlagViewerSizeAcks,
        "authenticated descriptor carries negotiated resize acknowledgements");

  const std::optional<cmux::TerminalHostFrame> frame =
      ReadFrameWithTimeout(descriptor);
  Check(frame && frame->kind == cmux::TerminalHostMessageKind::kSnapshot,
        "broker consumes exactly HostHello and leaves Snapshot unread");
  if (frame) {
    cmux::TerminalHostSnapshot snapshot;
    Check(
        cmux::DecodeTerminalHostSnapshot(
            std::string_view(
                reinterpret_cast<const char*>(frame->payload.data()),
                frame->payload.size()),
            &snapshot) == cmux::TerminalHostProtocolError::kNone &&
            snapshot.cols == 123 && snapshot.rows == 45 &&
            snapshot.replay == std::vector<uint8_t>({'s', 'n', 'a', 'p'}),
        "unread Snapshot payload remains complete after coalesced server send");
  }
  const int taken = result.socket->TakeDescriptor();
  Check(taken == descriptor && !result.socket->valid(),
        "descriptor ownership transfers exactly once");
  close(taken);

  result = RunCase(ResponsePlan::kLegacyWithSnapshot,
                   cmux::TerminalHostConnectError::kNone, 2);
  Check(result.socket && result.socket->protocol_flags() == 0,
        "old host without acknowledgement echo remains a legacy connection");
  if (result.socket) {
    close(result.socket->TakeDescriptor());
  }
}

void TestHandshakeFailures() {
  uint32_t sequence = 10;
  for (const auto& [plan, error] :
       std::vector<std::pair<ResponsePlan, cmux::TerminalHostConnectError>>{
           {ResponsePlan::kSelectedVersion,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kHeaderVersion,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kHeaderKind,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kHeaderFlags,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kHeaderRequest,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kHeaderSequence,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kReservedPayload,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kTerminalIdentity,
            cmux::TerminalHostConnectError::kIdentityMismatch},
           {ResponsePlan::kIncarnation,
            cmux::TerminalHostConnectError::kIdentityMismatch},
           {ResponsePlan::kRights,
            cmux::TerminalHostConnectError::kRightsMismatch},
           {ResponsePlan::kOversizePayload,
            cmux::TerminalHostConnectError::kInvalidHostHello},
           {ResponsePlan::kTruncatedHeader,
            cmux::TerminalHostConnectError::kReadFailed},
           {ResponsePlan::kStall, cmux::TerminalHostConnectError::kTimedOut},
       }) {
    cmux::TerminalHostConnectResult result = RunCase(plan, error, sequence++);
    Check(!result.socket, "failed handshake never leaks a descriptor");
    Check(!result.message.empty(), "failed handshake has diagnostic text");
  }
}

void TestCancellation() {
  cmux::TerminalHostRendererGrant grant = TestGrant(100);
  TestListener listener = Listen(grant);
  Check(listener.descriptor >= 0, "cancellation listener starts");
  std::atomic_bool observed(false);
  std::thread server(Serve, listener.descriptor, grant, ResponsePlan::kStall,
                     &observed);
  auto cancellation = std::make_shared<cmux::TerminalHostConnectCancellation>();
  auto cancelled_at = std::chrono::steady_clock::time_point();
  std::thread canceller([cancellation, &cancelled_at] {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    cancelled_at = std::chrono::steady_clock::now();
    cancellation->Cancel();
  });
  cmux::TerminalHostConnectResult result =
      cmux::ConnectAndAuthenticateTerminalHost(grant, std::chrono::seconds(2),
                                               cancellation);
  const auto completed_at = std::chrono::steady_clock::now();
  canceller.join();
  server.join();
  Check(result.error == cmux::TerminalHostConnectError::kCancelled,
        "in-flight handshake observes cancellation");
  // A loaded runner may not schedule the canceller at the requested 40 ms.
  // Measure the contract from the instant cancellation is actually published,
  // not from before that independent thread begins its scheduling delay.
  Check(completed_at >= cancelled_at &&
            completed_at - cancelled_at < std::chrono::milliseconds(200),
        "cancellation interrupts sliced poll promptly");

  auto pre_cancelled =
      std::make_shared<cmux::TerminalHostConnectCancellation>();
  pre_cancelled->Cancel();
  result = cmux::ConnectAndAuthenticateTerminalHost(
      TestGrant(101), std::chrono::seconds(2), pre_cancelled);
  Check(result.error == cmux::TerminalHostConnectError::kCancelled,
        "pre-cancelled broker avoids filesystem and socket work");
}

void TestEndpointConstraints() {
  cmux::TerminalHostRendererGrant grant = TestGrant(200);
  auto cancellation = std::make_shared<cmux::TerminalHostConnectCancellation>();
  cmux::TerminalHostConnectResult result =
      cmux::ConnectAndAuthenticateTerminalHost(
          grant, std::chrono::milliseconds(100), cancellation);
  Check(result.error == cmux::TerminalHostConnectError::kInvalidEndpointPath,
        "missing endpoint is rejected before connect");

  grant.endpoint = "/tmp/not-cmux/" +
                   cmux::EncodeTerminalHostId(grant.terminal_id) + ".sock";
  result = cmux::ConnectAndAuthenticateTerminalHost(
      grant, std::chrono::milliseconds(100), cancellation);
  Check(result.error == cmux::TerminalHostConnectError::kInvalidEndpointPath,
        "endpoint outside per-user root is rejected");

  grant = TestGrant(201);
  unlink(grant.endpoint.c_str());
  const int file =
      open(grant.endpoint.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  Check(file >= 0, "regular endpoint fixture is created");
  if (file >= 0) {
    close(file);
  }
  result = cmux::ConnectAndAuthenticateTerminalHost(
      grant, std::chrono::milliseconds(100), cancellation);
  Check(result.error == cmux::TerminalHostConnectError::kInvalidEndpointType,
        "regular file cannot impersonate terminal-host socket");
  unlink(grant.endpoint.c_str());

  grant = TestGrant(202);
  TestListener insecure = Listen(grant, 0666);
  Check(insecure.descriptor >= 0, "insecure socket fixture starts");
  result = cmux::ConnectAndAuthenticateTerminalHost(
      grant, std::chrono::milliseconds(100), cancellation);
  Check(result.error ==
            cmux::TerminalHostConnectError::kInvalidEndpointPermissions,
        "group/world-accessible terminal-host socket is rejected");

  grant = TestGrant(203);
  TestListener refused = Listen(grant);
  Check(refused.descriptor >= 0, "refused socket fixture starts");
  if (refused.descriptor >= 0) {
    close(refused.descriptor);
    refused.descriptor = -1;
  }
  result = cmux::ConnectAndAuthenticateTerminalHost(
      grant, std::chrono::milliseconds(100), cancellation);
  Check(result.error == cmux::TerminalHostConnectError::kConnectFailed,
        "stale secure socket path reports connect failure");

  result = cmux::ConnectAndAuthenticateTerminalHost(
      TestGrant(204), std::chrono::milliseconds(0), cancellation);
  Check(result.error == cmux::TerminalHostConnectError::kTimedOut,
        "nonpositive connection budget fails without blocking");
}

#endif  // !defined(_WIN32)

}  // namespace

int main() {
#if defined(_WIN32)
  std::printf("cmux-terminal-host-connection: skipped on Windows\n");
#else
  Check(PrepareHostRoot(), "private terminal-host root is available");
  TestSuccessfulHandshakeAndSnapshotBoundary();
  TestHandshakeFailures();
  TestCancellation();
  TestEndpointConstraints();
#endif
  std::printf("cmux-terminal-host-connection: %d checks, %d failures\n", checks,
              failures);
  return failures == 0 ? 0 : 1;
}
