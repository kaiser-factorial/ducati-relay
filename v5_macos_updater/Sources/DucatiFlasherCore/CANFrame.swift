import Foundation

public struct CANFrame: Equatable, Sendable {
    public let identifier: UInt32
    public let isExtended: Bool
    public let isRemote: Bool
    public let data: [UInt8]

    public init(identifier: UInt32, isExtended: Bool = false, isRemote: Bool = false, data: [UInt8]) {
        self.identifier = identifier
        self.isExtended = isExtended
        self.isRemote = isRemote
        self.data = data
    }

    public var slcanEncoded: String {
        let type: Character
        switch (isExtended, isRemote) {
        case (false, false): type = "t"
        case (true, false): type = "T"
        case (false, true): type = "r"
        case (true, true): type = "R"
        }

        let identifierWidth = isExtended ? 8 : 3
        let identifierText = String(format: "%0*lX", identifierWidth, identifier)
        let payload = isRemote ? "" : data.map { String(format: "%02X", $0) }.joined()
        return "\(type)\(identifierText)\(String(format: "%X", data.count))\(payload)\r"
    }

    public static func parseSLCAN(_ line: String) throws -> CANFrame {
        guard let type = line.first, "tTrR".contains(type) else {
            throw FlasherError.protocolError("Not an SLCAN CAN frame: \(line)")
        }

        let extended = type == "T" || type == "R"
        let remote = type == "r" || type == "R"
        let identifierWidth = extended ? 8 : 3
        let characters = Array(line)
        let dlcIndex = 1 + identifierWidth
        guard characters.count > dlcIndex else {
            throw FlasherError.protocolError("Truncated SLCAN frame")
        }

        guard let identifier = UInt32(String(characters[1..<dlcIndex]), radix: 16),
              let dlc = Int(String(characters[dlcIndex]), radix: 16),
              dlc <= 8 else {
            throw FlasherError.protocolError("Invalid SLCAN identifier or DLC")
        }

        let expectedCount = dlcIndex + 1 + (remote ? 0 : dlc * 2)
        guard characters.count == expectedCount else {
            throw FlasherError.protocolError("SLCAN payload length does not match DLC")
        }

        var data: [UInt8] = []
        if !remote {
            for index in 0..<dlc {
                let start = dlcIndex + 1 + index * 2
                guard let byte = UInt8(String(characters[start..<(start + 2)]), radix: 16) else {
                    throw FlasherError.protocolError("Invalid SLCAN payload byte")
                }
                data.append(byte)
            }
        } else {
            data = Array(repeating: 0, count: dlc)
        }

        return CANFrame(identifier: identifier, isExtended: extended, isRemote: remote, data: data)
    }
}

public enum FlasherError: Error, CustomStringConvertible, Equatable {
    case invalidArgument(String)
    case io(String)
    case protocolError(String)
    case unsafe(String)

    public var description: String {
        switch self {
        case .invalidArgument(let message): return message
        case .io(let message): return message
        case .protocolError(let message): return message
        case .unsafe(let message): return message
        }
    }
}
