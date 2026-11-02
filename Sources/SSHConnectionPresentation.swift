import CmuxCloudTui
import CmuxCore

/// Read-only SSH presentation derived from intent, carrier and pane attachments.
struct SSHConnectionPresentation {
    struct Attachment {
        let phase: CloudTuiManualMirrorPhase
        let allowsAutomaticReconnect: Bool
    }

    let resumeEnabled: Bool
    let controllerState: WorkspaceRemoteConnectionState
    let carrierConnected: Bool?
    let requiresManualAuthentication: Bool
    let attachments: [Attachment]

    var state: WorkspaceRemoteConnectionState {
        if !resumeEnabled { return .disconnected }
        if requiresManualAuthentication,
           carrierConnected != true || attachments.isEmpty || attachments.contains(where: { $0.phase != .attached }) {
            return .error
        }
        if !attachments.isEmpty {
            if attachments.contains(where: { $0.allowsAutomaticReconnect && $0.phase != .attached && $0.phase != .stopped }) {
                return recoveryState
            }
            if attachments.contains(where: { $0.phase == .attached }) {
                return carrierConnected == false ? recoveryState : .connected
            }
            return .disconnected
        }
        if carrierConnected == false, controllerState == .connected { return .reconnecting }
        return controllerState
    }

    func state(for attachment: Attachment) -> WorkspaceRemoteConnectionState {
        if !resumeEnabled { return .disconnected }
        if requiresManualAuthentication, attachment.phase != .attached || carrierConnected == false { return .error }
        switch attachment.phase {
        case .attached: return carrierConnected == false ? recoveryState : .connected
        case .connecting, .idle, .disconnected:
            return attachment.allowsAutomaticReconnect ? recoveryState : .disconnected
        case .stopped: return .disconnected
        }
    }

    private var recoveryState: WorkspaceRemoteConnectionState {
        controllerState == .connecting ? .connecting : .reconnecting
    }
}
