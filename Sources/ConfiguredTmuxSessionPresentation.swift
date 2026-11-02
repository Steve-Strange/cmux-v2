import Foundation

/// Names only an exact, existing-session attach; arbitrary remote commands remain opaque.
struct ConfiguredTmuxSessionPresentation {
    let sessionName: String

    init?(arguments: [String]) {
        guard arguments.count == 4,
              (arguments[0] as NSString).lastPathComponent == "tmux",
              ["attach", "attach-session", "a"].contains(arguments[1]),
              arguments[2] == "-t",
              arguments[3].hasPrefix("=") else { return nil }
        let name = String(arguments[3].dropFirst())
        guard !name.isEmpty,
              !name.contains(":"),
              name.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) else { return nil }
        sessionName = name
    }
}
