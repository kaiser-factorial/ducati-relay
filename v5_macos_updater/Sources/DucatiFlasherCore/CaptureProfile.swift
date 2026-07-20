import Foundation

public struct CaptureFrame: Codable, Equatable, Sendable {
    public let timestamp: Double
    public let identifier: UInt32
    public let dataHex: String
}

public struct ExchangeProfile: Codable, Equatable, Sendable {
    public let formatVersion: Int
    public let createdAt: String
    public let sourceCapture: String
    public let requestIdentifier: UInt32
    public let responseIdentifier: UInt32
    public let requestFrameCount: Int
    public let responseFrameCount: Int
    public let exchangeDurationSeconds: Double
    public let firstRequest: CaptureFrame
    public let firstResponse: CaptureFrame
    public let liveFlashingEnabled: Bool
    public let validationState: String
}

public enum CaptureImporter {
    private static let expression = try! NSRegularExpression(
        pattern: #"^\((\d+\.\d+)\)\s+\S+\s+([0-9A-Fa-f]+)#([0-9A-Fa-f]*)$"#
    )

    public static func parse(_ contents: String) throws -> [CaptureFrame] {
        try contents.split(whereSeparator: \Character.isNewline).compactMap { rawLine in
            let line = String(rawLine).trimmingCharacters(in: .whitespacesAndNewlines)
            guard !line.isEmpty else { return nil }
            let range = NSRange(line.startIndex..<line.endIndex, in: line)
            guard let match = expression.firstMatch(in: line, range: range), match.numberOfRanges == 4,
                  let timestampRange = Range(match.range(at: 1), in: line),
                  let identifierRange = Range(match.range(at: 2), in: line),
                  let dataRange = Range(match.range(at: 3), in: line),
                  let timestamp = Double(line[timestampRange]),
                  let identifier = UInt32(line[identifierRange], radix: 16) else {
                throw FlasherError.protocolError("Unrecognized capture line: \(line)")
            }
            return CaptureFrame(
                timestamp: timestamp,
                identifier: identifier,
                dataHex: String(line[dataRange]).uppercased()
            )
        }
    }

    public static func makeProfile(contents: String, source: String) throws -> ExchangeProfile {
        let frames = try parse(contents)
        let requests = frames.filter { $0.identifier == 0x667 }
        let responses = frames.filter { $0.identifier == 0x7E1 }
        guard let firstRequest = requests.first else {
            throw FlasherError.protocolError("Capture contains no OpenBLT requests on 0x667")
        }
        guard let firstResponse = responses.first else {
            throw FlasherError.protocolError("Capture contains no OpenBLT responses on 0x7E1")
        }
        let all = (requests + responses).sorted { $0.timestamp < $1.timestamp }
        let duration = (all.last?.timestamp ?? 0) - (all.first?.timestamp ?? 0)
        let formatter = ISO8601DateFormatter()

        return ExchangeProfile(
            formatVersion: 1,
            createdAt: formatter.string(from: Date()),
            sourceCapture: source,
            requestIdentifier: 0x667,
            responseIdentifier: 0x7E1,
            requestFrameCount: requests.count,
            responseFrameCount: responses.count,
            exchangeDurationSeconds: duration,
            firstRequest: firstRequest,
            firstResponse: firstResponse,
            liveFlashingEnabled: false,
            validationState: "captured-unvalidated"
        )
    }

    public static func loadProfile(path: String) throws -> ExchangeProfile {
        do {
            let data = try Data(contentsOf: URL(fileURLWithPath: path))
            return try JSONDecoder().decode(ExchangeProfile.self, from: data)
        } catch {
            throw FlasherError.io("Unable to load exchange profile: \(error.localizedDescription)")
        }
    }

    public static func writeProfile(_ profile: ExchangeProfile, path: String) throws {
        do {
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            let data = try encoder.encode(profile)
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
        } catch {
            throw FlasherError.io("Unable to write exchange profile: \(error.localizedDescription)")
        }
    }
}
