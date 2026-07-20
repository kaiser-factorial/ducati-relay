import DucatiFlasherCore
import Foundation

private var failures = 0

@MainActor private func check(_ condition: @autoclosure () throws -> Bool, _ name: String) {
    do {
        if try condition() {
            print("PASS \(name)")
        } else {
            failures += 1
            print("FAIL \(name)")
        }
    } catch {
        failures += 1
        print("FAIL \(name): \(error)")
    }
}

@MainActor private func expectThrow(_ name: String, _ operation: () throws -> Void) {
    do {
        try operation()
        failures += 1
        print("FAIL \(name): expected an error")
    } catch {
        print("PASS \(name)")
    }
}

private func makeRecord(type: Int, address: [UInt8], data: [UInt8]) -> String {
    let count = UInt8(address.count + data.count + 1)
    let withoutChecksum = [count] + address + data
    let checksum = UInt8(0xFF - (withoutChecksum.reduce(0) { ($0 + Int($1)) & 0xFF }))
    return "S\(type)" + (withoutChecksum + [checksum]).map { String(format: "%02X", $0) }.joined()
}

let standardFrame = CANFrame(identifier: 0x667, data: [0xFF, 0x00, 0x12])
check(standardFrame.slcanEncoded == "t6673FF0012\r", "SLCAN standard encoding")
check(try CANFrame.parseSLCAN("t6673FF0012") == standardFrame, "SLCAN standard round trip")

let extendedFrame = CANFrame(identifier: 0x18DAF110, isExtended: true, data: [0x01])
check(try CANFrame.parseSLCAN("T18DAF110101") == extendedFrame, "SLCAN extended round trip")

let validSRecord = makeRecord(type: 3, address: [0x08, 0, 0, 0], data: [1, 2, 3, 4])
    + "\n" + makeRecord(type: 7, address: [0x08, 0, 0, 0], data: [])
let summary = try SRecordParser.parse(validSRecord)
check(summary.dataRecordCount == 1, "S-record data record count")
check(summary.dataByteCount == 4, "S-record byte count")
check(summary.lowestAddress == 0x08000000, "S-record low address")
check(summary.highestAddress == 0x08000003, "S-record high address")

let invalidSRecord = String(validSRecord.dropLast()) + "0"
expectThrow("S-record checksum rejection") { _ = try SRecordParser.parse(invalidSRecord) }

let capture = """
(1784533000.100000) slcan0 667#FF00
(1784533000.125000) slcan0 7E1#FF00
"""
let profile = try CaptureImporter.makeProfile(contents: capture, source: "fixture.log")
check(profile.requestFrameCount == 1, "capture request count")
check(profile.responseFrameCount == 1, "capture response count")
check(!profile.liveFlashingEnabled, "capture profile safety lock")

expectThrow("one-way capture rejection") {
    _ = try CaptureImporter.makeProfile(
        contents: "(1784533000.100000) slcan0 667#FF00\n",
        source: "fixture.log"
    )
}

if failures > 0 {
    print("\n\(failures) self-test(s) failed")
    exit(1)
}
print("\nAll native updater self-tests passed")
