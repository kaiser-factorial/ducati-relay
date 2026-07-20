import DucatiFlasherCore
import Foundation

private let version = "0.1.0-placeholder"

private func printUsage() {
    print("""
    ducati-flasher \(version)

    Native macOS foundation for the rusEFI nano CAN updater.

    Commands:
      ports
          List likely ESP32 USB serial ports.

      doctor [--port /dev/cu.usbserial-...]
          Check the local tool and optionally query/configure the SLCAN bridge.

      validate <firmware.srec>
          Validate S-record structure, byte counts, checksums, and address range.

      import-capture <openblt_exchange.log> --output <profile.json>
          Convert Route 1's successful capture into a locked Route 2 profile.

      capture --port <port> --output <log> [--seconds <n>]
          Capture raw OpenBLT 0x667/0x7E1 traffic directly on macOS.

      dry-run --firmware <file> --profile <profile.json>
          Validate every available input without sending programming commands.

      flash --port <port> --firmware <file> --profile <profile.json>
          Safety-locked placeholder until the exchange is captured and XCP is validated.
    """)
}

private func value(after option: String, in arguments: [String]) throws -> String {
    guard let index = arguments.firstIndex(of: option), index + 1 < arguments.count else {
        throw FlasherError.invalidArgument("Missing required option: \(option)")
    }
    return arguments[index + 1]
}

private func display(_ summary: SRecordSummary) {
    print("S-record valid")
    print("  Records:      \(summary.recordCount)")
    print("  Data records: \(summary.dataRecordCount)")
    print("  Data bytes:   \(summary.dataByteCount)")
    print(String(format: "  Address range: 0x%08X–0x%08X", summary.lowestAddress, summary.highestAddress))
    print("  Termination:  \(summary.terminationRecordType)")
}

private func runDoctor(arguments: [String]) throws {
    print("ducati-flasher \(version)")
    print("Platform: macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
    let ports = SerialSLCAN.candidatePorts()
    print("Candidate serial ports: \(ports.isEmpty ? "none" : ports.joined(separator: ", "))")

    guard arguments.contains("--port") else {
        print("Local checks passed. Add --port to test the ESP32 bridge.")
        return
    }

    let path = try value(after: "--port", in: arguments)
    let adapter = SerialSLCAN(path: path)
    try adapter.openPort()
    let adapterVersion = try adapter.adapterVersion()
    guard adapterVersion.hasPrefix("V") else {
        throw FlasherError.protocolError("Unexpected adapter version response: \(adapterVersion)")
    }
    try adapter.configure500K()
    print("SLCAN bridge: \(adapterVersion)")
    print("CAN channel: open at 500 kbit/s")
}

private func runImport(arguments: [String]) throws {
    guard let source = arguments.first, !source.hasPrefix("--") else {
        throw FlasherError.invalidArgument("import-capture requires an openblt_exchange.log path")
    }
    let output = try value(after: "--output", in: arguments)
    let contents: String
    do {
        contents = try String(contentsOfFile: source, encoding: .utf8)
    } catch {
        throw FlasherError.io("Unable to read capture: \(error.localizedDescription)")
    }
    let profile = try CaptureImporter.makeProfile(contents: contents, source: source)
    try CaptureImporter.writeProfile(profile, path: output)
    print("Imported bidirectional OpenBLT exchange")
    print("  Requests 0x667:  \(profile.requestFrameCount)")
    print("  Responses 0x7E1: \(profile.responseFrameCount)")
    print(String(format: "  Duration:          %.3f seconds", profile.exchangeDurationSeconds))
    print("  Profile:           \(output)")
    print("  Live flashing:     LOCKED pending protocol validation")
}

private func runCapture(arguments: [String]) throws {
    let port = try value(after: "--port", in: arguments)
    let output = try value(after: "--output", in: arguments)
    let duration = arguments.contains("--seconds")
        ? Double(try value(after: "--seconds", in: arguments)) ?? 30
        : 30
    guard duration > 0 else { throw FlasherError.invalidArgument("--seconds must be positive") }

    let adapter = SerialSLCAN(path: port)
    try adapter.openPort()
    try adapter.configure500K()
    let deadline = Date().addingTimeInterval(duration)
    var lines: [String] = []
    print("Capturing 0x667/0x7E1 for \(duration) seconds...")

    while Date() < deadline {
        guard let frame = try adapter.receive(timeout: 0.25) else { continue }
        guard frame.identifier == 0x667 || frame.identifier == 0x7E1 else { continue }
        let data = frame.data.map { String(format: "%02X", $0) }.joined()
        let line = String(format: "(%.6f) macslcan %03X#%@", Date().timeIntervalSince1970, frame.identifier, data)
        lines.append(line)
        print(line)
    }

    try (lines.joined(separator: "\n") + (lines.isEmpty ? "" : "\n"))
        .write(toFile: output, atomically: true, encoding: .utf8)
    print("Saved \(lines.count) filtered frame(s) to \(output)")
}

private func runDryRun(arguments: [String]) throws {
    let firmwarePath = try value(after: "--firmware", in: arguments)
    let profilePath = try value(after: "--profile", in: arguments)
    let summary = try SRecordParser.parse(fileAt: firmwarePath)
    let profile = try CaptureImporter.loadProfile(path: profilePath)

    guard profile.requestIdentifier == 0x667, profile.responseIdentifier == 0x7E1 else {
        throw FlasherError.unsafe("Profile does not use the expected OpenBLT CAN identifiers")
    }
    print("DRY RUN ONLY — no serial port opened and no CAN traffic sent")
    display(summary)
    print("Exchange profile valid")
    print("  Requests:  \(profile.requestFrameCount)")
    print("  Responses: \(profile.responseFrameCount)")
    print("  State:     \(profile.validationState)")
    if profile.requestFrameCount == 0 || profile.responseFrameCount == 0 {
        print("  Capture:   PLACEHOLDER — import the successful Route 1 exchange next")
    }
    print("Live XCP engine: LOCKED")
}

private func runFlash(arguments: [String]) throws {
    _ = try value(after: "--port", in: arguments)
    let firmwarePath = try value(after: "--firmware", in: arguments)
    let profilePath = try value(after: "--profile", in: arguments)
    _ = try SRecordParser.parse(fileAt: firmwarePath)
    let profile = try CaptureImporter.loadProfile(path: profilePath)

    guard profile.liveFlashingEnabled else {
        throw FlasherError.unsafe(
            "LIVE FLASH REFUSED: this placeholder profile is capture-only. " +
            "Import the first successful Route 1 exchange, implement/validate XCP sequencing, " +
            "and pass hardware replay tests before this gate may be changed."
        )
    }
    throw FlasherError.unsafe("LIVE FLASH REFUSED: the native XCP programming engine is not implemented yet")
}

do {
    let arguments = Array(CommandLine.arguments.dropFirst())
    guard let command = arguments.first else {
        printUsage()
        exit(0)
    }
    let rest = Array(arguments.dropFirst())

    switch command {
    case "help", "--help", "-h":
        printUsage()
    case "ports":
        let ports = SerialSLCAN.candidatePorts()
        if ports.isEmpty { print("No likely ESP32 serial ports found.") }
        else { ports.forEach { print($0) } }
    case "doctor":
        try runDoctor(arguments: rest)
    case "validate":
        guard let path = rest.first else { throw FlasherError.invalidArgument("validate requires a firmware path") }
        display(try SRecordParser.parse(fileAt: path))
    case "import-capture":
        try runImport(arguments: rest)
    case "capture":
        try runCapture(arguments: rest)
    case "dry-run":
        try runDryRun(arguments: rest)
    case "flash":
        try runFlash(arguments: rest)
    default:
        throw FlasherError.invalidArgument("Unknown command: \(command). Run ducati-flasher help.")
    }
} catch {
    FileHandle.standardError.write(Data("ERROR: \(error)\n".utf8))
    exit(1)
}
