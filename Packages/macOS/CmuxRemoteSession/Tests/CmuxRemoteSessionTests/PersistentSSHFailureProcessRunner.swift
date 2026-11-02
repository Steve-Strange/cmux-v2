@testable import CmuxRemoteSession

/// Models a failed SSH network or authentication attempt without opening a socket.
struct PersistentSSHFailureProcessRunner: RemoteSessionProcessRunning {
    let detail: String

    func run(
        _ request: RemoteProcessRequest,
        operation: (any RemoteTransferCancelling)?
    ) throws -> RemoteCommandResult {
        RemoteCommandResult(status: 255, stdout: "", stderr: detail)
    }
}
