import Darwin
import Foundation

/// Resolves a foreground tmux client to its current session without creating a server.
struct TmuxAttachedSessionResolver: Sendable {
    struct AttachedSession: Equatable, Sendable {
        let socketPath: String
        let sessionID: String
        let sessionName: String
    }

    var timeout: TimeInterval = 0.5
    var temporaryDirectory = FileManager.default.temporaryDirectory

    func resolve(
        processID: Int,
        executablePath: String,
        socketFlags: [String],
        environment: [String: String]
    ) -> AttachedSession? {
        guard processID > 0, executablePath.hasPrefix("/") else { return nil }
        let format = "#{client_pid}\t#{session_id}\t#{session_name}\t#{socket_path}"
        // -N forbids starting a server; -u preserves the tab separators even
        // when the app or observed client has no UTF-8 locale configured.
        guard let output = query(
            executablePath: executablePath,
            arguments: ["-N", "-u"] + socketFlags + ["list-clients", "-F", format],
            environment: environment
        ) else { return nil }
        var matches: [AttachedSession] = []
        for line in output.split(separator: "\n") {
            let fields = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            guard fields.count == 4, Int(fields[0]) == processID,
                  fields[1].range(of: "^\\$[0-9]+$", options: .regularExpression) != nil,
                  !fields[2].isEmpty, fields[3].hasPrefix("/"),
                  !fields[2...3].contains(where: containsControlCharacters) else { continue }
            matches.append(AttachedSession(socketPath: fields[3], sessionID: fields[1], sessionName: fields[2]))
        }
        guard matches.count == 1 else { return nil }
        return matches[0]
    }

    private func containsControlCharacters(_ value: String) -> Bool {
        value.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
    }

    /// This synchronous process scan already runs on a worker. Capture into an
    /// unlinked file so output cannot fill a pipe while the deadline owns exit.
    private func query(executablePath: String, arguments: [String], environment: [String: String]) -> String? {
        var template = Array(temporaryDirectory.appendingPathComponent("cmux-tmux-client-XXXXXX").path.utf8CString)
        let descriptor = template.withUnsafeMutableBufferPointer { mkstemp($0.baseAddress!) }
        guard descriptor >= 0 else { return nil }
        template.withUnsafeBufferPointer { _ = unlink($0.baseAddress!) }
        let output = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        defer { try? output.close() }
        _ = fcntl(descriptor, F_SETFD, FD_CLOEXEC)

        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = arguments
        var queryEnvironment = ["PATH": "/usr/bin:/bin", "LC_CTYPE": "UTF-8"]
        for key in ["HOME", "TMUX_TMPDIR", "TMPDIR"] {
            queryEnvironment[key] = environment[key]
        }
        process.environment = queryEnvironment
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        do { try process.run() } catch { return nil }

        // A genuine external-command deadline is needed at this synchronous
        // boundary; the scanner has no async task in which to host Clock.sleep.
        let deadline = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        deadline.schedule(deadline: .now() + max(0.01, timeout))
        deadline.setEventHandler {
            guard process.isRunning else { return }
            process.terminate()
            _ = Darwin.kill(process.processIdentifier, SIGKILL)
        }
        deadline.resume()
        defer { deadline.cancel() }
        process.waitUntilExit()
        guard process.terminationStatus == 0 else { return nil }
        do {
            try output.seek(toOffset: 0)
            let maximumBytes = 64 * 1024
            guard let data = try output.read(upToCount: maximumBytes + 1), data.count <= maximumBytes else { return nil }
            return String(data: data, encoding: .utf8)
        } catch {
            return nil
        }
    }
}
