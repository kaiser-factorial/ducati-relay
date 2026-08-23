import Foundation

public struct SRecordSummary: Codable, Equatable, Sendable {
    public let recordCount: Int
    public let dataRecordCount: Int
    public let dataByteCount: Int
    public let lowestAddress: UInt32
    public let highestAddress: UInt32
    public let terminationRecordType: String
}

public enum SRecordParser {
    public static func parse(fileAt path: String) throws -> SRecordSummary {
        let contents: String
        do {
            contents = try String(contentsOfFile: path, encoding: .utf8)
        } catch {
            throw FlasherError.io("Unable to read firmware: \(path): \(error.localizedDescription)")
        }
        return try parse(contents)
    }

    public static func parse(_ contents: String) throws -> SRecordSummary {
        let lines = contents.split(whereSeparator: \Character.isNewline).map(String.init)
        guard !lines.isEmpty else { throw FlasherError.protocolError("S-record file is empty") }

        var dataRecordCount = 0
        var dataByteCount = 0
        var lowestAddress = UInt32.max
        var highestAddress: UInt32 = 0
        var terminationType: String?

        for (offset, rawLine) in lines.enumerated() {
            let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            guard line.count >= 4, line.first == "S" else {
                throw FlasherError.protocolError("Line \(offset + 1) is not an S-record")
            }

            let characters = Array(line)
            let type = characters[1]
            guard type.isNumber else {
                throw FlasherError.protocolError("Line \(offset + 1) has an invalid record type")
            }

            let hexText = String(characters.dropFirst(2))
            guard hexText.count.isMultiple(of: 2) else {
                throw FlasherError.protocolError("Line \(offset + 1) has an odd number of hex digits")
            }

            var bytes: [UInt8] = []
            var index = hexText.startIndex
            while index < hexText.endIndex {
                let next = hexText.index(index, offsetBy: 2)
                guard let byte = UInt8(hexText[index..<next], radix: 16) else {
                    throw FlasherError.protocolError("Line \(offset + 1) contains non-hex data")
                }
                bytes.append(byte)
                index = next
            }

            guard let count = bytes.first, Int(count) == bytes.count - 1 else {
                throw FlasherError.protocolError("Line \(offset + 1) has an invalid byte count")
            }
            let checksum = bytes.reduce(0) { (Int($0) + Int($1)) & 0xFF }
            guard checksum == 0xFF else {
                throw FlasherError.protocolError("Line \(offset + 1) has an invalid checksum")
            }

            if "123".contains(type) {
                let addressBytes = type == "1" ? 2 : (type == "2" ? 3 : 4)
                guard bytes.count >= addressBytes + 2 else {
                    throw FlasherError.protocolError("Line \(offset + 1) is too short")
                }
                var address: UInt32 = 0
                for byte in bytes[1...addressBytes] {
                    address = (address << 8) | UInt32(byte)
                }
                let payloadCount = Int(count) - addressBytes - 1
                dataRecordCount += 1
                dataByteCount += payloadCount
                lowestAddress = min(lowestAddress, address)
                if payloadCount > 0 {
                    highestAddress = max(highestAddress, address + UInt32(payloadCount - 1))
                }
            } else if "789".contains(type) {
                terminationType = "S\(type)"
            }
        }

        guard dataRecordCount > 0 else {
            throw FlasherError.protocolError("S-record contains no data records")
        }
        guard let terminationType else {
            throw FlasherError.protocolError("S-record has no S7/S8/S9 termination record")
        }

        return SRecordSummary(
            recordCount: lines.count,
            dataRecordCount: dataRecordCount,
            dataByteCount: dataByteCount,
            lowestAddress: lowestAddress,
            highestAddress: highestAddress,
            terminationRecordType: terminationType
        )
    }
}
