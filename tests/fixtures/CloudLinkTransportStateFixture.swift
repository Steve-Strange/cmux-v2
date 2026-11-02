import Darwin
import Foundation

private actor CloudLinkStateObservations {
    struct Observation: Sendable {
        let state: SurfaceLinkState
        let change: CloudMachineLink.Change
    }
    private var observations: [Observation] = []
    private var waiters: [(after: Int, predicate: @Sendable (Observation) -> Bool,
                           result: CloudLinkFirstValue<Bool>)] = []
    var count: Int { observations.count }
    func record(state: SurfaceLinkState, change: CloudMachineLink.Change) {
        observations.append(Observation(state: state, change: change))
        for waiter in waiters where observations.count > waiter.after && waiter.predicate(observations.last!) {
            waiter.result.resolve(true)
        }
    }
    func wait(after: Int, predicate: @escaping @Sendable (Observation) -> Bool) async -> Bool {
        if observations.dropFirst(after).contains(where: predicate) { return true }
        let value = CloudLinkFirstValue<Bool>()
        waiters.append((after, predicate, value))
        return await withTaskGroup(of: Bool.self) { group in
            group.addTask { await value.result ?? false }
            group.addTask { try? await Task.sleep(for: .seconds(1)); return false }
            let result = await group.next() ?? false
            group.cancelAll()
            return result
        }
    }
}

@main
struct CloudLinkTransportStateFixture {
    struct Failure: Error, CustomStringConvertible { let description: String }

    static func require(_ condition: Bool, _ message: String) throws {
        if !condition { throw Failure(description: message) }
    }

    static func main() async {
        do {
            try await exercise(initialState: "connected")
            try await exercise(initialState: "reconnecting")
            try await exercise(initialState: "legacy")
            print("PASS initial, full graph, physical loss, socket reuse, recovery and process exit")
        } catch {
            print("FAIL \(error)")
            exit(1)
        }
    }

    static func exercise(initialState: String) async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("cmux-link-state-\(UUID())")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let fifo = root.appendingPathComponent("commands")
        guard mkfifo(fifo.path, 0o600) == 0 else { throw POSIXError(.EIO) }
        let client = root.appendingPathComponent("client")
        let pidPath = root.appendingPathComponent("pid")
        try """
        #!/usr/bin/env python3
        import json,os,sys
        command = os.fdopen(os.open('\(fifo.path)', os.O_RDWR))
        with open('\(pidPath.path)', 'a') as p: p.write(str(os.getpid())+'\\n')
        socket = '/tmp/cmux-fixture-\(UUID().uuidString.prefix(8)).sock'
        def snapshot(state):
            value = {'event':'connection-snapshot','local_socket':socket}
            if state != 'legacy':
                value['connection'] = {'state':state,'generation':1,'physical_link_count':1,
                    'session_id':'fixture','lane_bindings':[['control']],
                    'transport':{'provider':'ssh','route':'ssh://fixture'}}
            print(json.dumps(value), flush=True)
        print('{"event":"unrelated","local_socket":"/tmp/wrong.sock"}', flush=True)
        snapshot('\(initialState)')
        for line in command:
            value = line.strip()
            if value == 'exit':
                print('fixture carrier exited', file=sys.stderr, flush=True)
                sys.exit(7)
            if value == 'invalid':
                print('{"event":"connection-snapshot","connection":{"state":"closed"}}', flush=True)
                print('{"event":"connection-snapshot","local_socket":"/tmp/wrong.sock","connection":{"state":"closed"}}', flush=True)
                continue
            snapshot(value)
        """.write(to: client, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: client.path)
        let link = CloudMachineLink(machineID: "fixture", clientURL: client, paths: CloudTuiClientPaths(home: root))
        let observations = CloudLinkStateObservations()
        let observer = Task {
            for await change in link.changes {
                await observations.record(state: link.state, change: change)
            }
        }
        let connected = try await link.connect(route: "ssh://fixture", session: "cmux", carrier: true)
        let writer = try FileHandle(forWritingTo: fifo)
        defer { try? writer.close() }
        do {
            let expectedInitial: SurfaceLinkState = initialState == "reconnecting" ? .connecting : .connected
            try require(await link.state == expectedInitial,
                        "first \(initialState) snapshot was presented as \(await link.state)")
            if initialState == "reconnecting" {
                let baseline = await observations.count
                try writer.write(contentsOf: Data("connected\n".utf8))
                try require(await observations.wait(after: baseline) { $0.state == .connected },
                            "first recovery was not published")
            }
            let baseline = await observations.count
            try writer.write(contentsOf: Data("reconnecting\n".utf8))
            try require(await observations.wait(after: baseline) { $0.state == .connecting },
                        "physical SSH loss never changed the link state from connected")
            try require(await link.isConnected == false, "recovering transport still reports connected")
            try require(await link.connected == connected, "network loss discarded the stable local socket")
            let reused = try await link.connect(route: "ssh://fixture", session: "cmux", carrier: true)
            try require(reused == connected, "an existing recovering carrier must be reused")
            let starts = try String(contentsOf: pidPath, encoding: .utf8).split(separator: "\n")
            try require(starts.count == 1, "network recovery spawned a replacement carrier")

            // A graph event on a retained stream must remain usable during a
            // physical outage without falsely declaring the transport connected.
            let beforeGraph = await observations.count
            await CloudLinkControlFixture.shared.emitSnapshot(revision: 10)
            try require(await observations.wait(after: beforeGraph) {
                if case .snapshot(let cursor, _, _) = $0.change { return cursor.revision == 10 }
                return false
            }, "a physical reconnect dropped the retained event stream")
            try require(await link.state == .connecting, "a full graph snapshot overrode physical reconnect state")
            do {
                _ = try await link.run(arguments: CloudTuiRequest("timeout"), timeout: .milliseconds(1))
            } catch { /* A failed request must never retire the recovering client. */ }
            try require(await link.connected == connected, "a recovering control request retired the carrier")

            let beforeRecovery = await observations.count
            try writer.write(contentsOf: Data("invalid\nconnected\n".utf8))
            try require(await observations.wait(after: beforeRecovery) { $0.state == .connected },
                        "physical recovery was not published")
            try require(await link.isConnected, "physical recovery did not restore connected")
            try require(await link.connected == connected, "recovery changed the local socket")

            let beforeExit = await observations.count
            try writer.write(contentsOf: Data("exit\n".utf8))
            try require(await observations.wait(after: beforeExit) { $0.state == .error },
                        "carrier exit was not published")
            try require(await link.connected == nil, "exited carrier retained its local socket")
            try require(await link.isConnected == false, "exited carrier retained ready state")
            await link.disconnect()
            await observer.value
        } catch {
            await link.disconnect()
            await observer.value
            throw error
        }
    }
}
