import Darwin
import Foundation

public final class SerialSLCAN: @unchecked Sendable {
    private let path: String
    private var descriptor: Int32 = -1
    private var receiveBuffer = Data()

    public init(path: String) {
        self.path = path
    }

    deinit {
        closePort()
    }

    public static func candidatePorts() -> [String] {
        let directory = "/dev"
        let prefixes = ["cu.usbserial", "cu.usbmodem", "cu.SLAB_USBtoUART", "cu.wchusbserial"]
        guard let entries = try? FileManager.default.contentsOfDirectory(atPath: directory) else { return [] }
        return entries.filter { entry in prefixes.contains { entry.hasPrefix($0) } }
            .sorted()
            .map { "\(directory)/\($0)" }
    }

    public func openPort() throws {
        guard descriptor < 0 else { return }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/stty")
        process.arguments = ["-f", path, "921600", "raw", "-echo"]
        let errorPipe = Pipe()
        process.standardError = errorPipe
        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            throw FlasherError.io("Unable to run stty: \(error.localizedDescription)")
        }
        guard process.terminationStatus == 0 else {
            let data = errorPipe.fileHandleForReading.readDataToEndOfFile()
            let message = String(data: data, encoding: .utf8) ?? "unknown stty error"
            throw FlasherError.io("Unable to configure \(path): \(message.trimmingCharacters(in: .whitespacesAndNewlines))")
        }

        descriptor = Darwin.open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard descriptor >= 0 else {
            throw FlasherError.io("Unable to open \(path): \(String(cString: strerror(errno)))")
        }
        let flags = fcntl(descriptor, F_GETFL)
        _ = fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK)
        tcflush(descriptor, TCIOFLUSH)
    }

    public func closePort() {
        if descriptor >= 0 {
            _ = Darwin.close(descriptor)
            descriptor = -1
        }
    }

    public func configure500K() throws {
        try sendCommand("C", expect: .acknowledgement, timeout: 1.0)
        try sendCommand("S6", expect: .acknowledgement, timeout: 1.0)
        try sendCommand("O", expect: .acknowledgement, timeout: 1.0)
    }

    public func adapterVersion() throws -> String {
        try writeLine("V")
        return try readLine(timeout: 1.0)
    }

    public func transmit(_ frame: CANFrame) throws {
        try writeRaw(frame.slcanEncoded)
        let response = try readLine(timeout: 1.0, includeEmptyAcknowledgement: true)
        guard response.isEmpty else {
            throw FlasherError.protocolError("SLCAN adapter rejected CAN frame: \(response)")
        }
    }

    public func receive(timeout: TimeInterval) throws -> CANFrame? {
        do {
            let line = try readLine(timeout: timeout)
            guard !line.isEmpty, "tTrR".contains(line.first!) else { return nil }
            return try CANFrame.parseSLCAN(line)
        } catch FlasherError.io(let message) where message == "Serial read timed out" {
            return nil
        }
    }

    private enum ExpectedResponse { case acknowledgement }

    private func sendCommand(_ command: String, expect: ExpectedResponse, timeout: TimeInterval) throws {
        try writeLine(command)
        let response = try readLine(timeout: timeout, includeEmptyAcknowledgement: true)
        guard response.isEmpty else {
            throw FlasherError.protocolError("SLCAN command \(command) was rejected: \(response)")
        }
    }

    private func writeLine(_ command: String) throws {
        try writeRaw(command + "\r")
    }

    private func writeRaw(_ text: String) throws {
        guard descriptor >= 0 else { throw FlasherError.io("Serial port is not open") }
        let bytes = Array(text.utf8)
        var written = 0
        while written < bytes.count {
            let result = bytes.withUnsafeBytes { pointer in
                Darwin.write(descriptor, pointer.baseAddress!.advanced(by: written), bytes.count - written)
            }
            guard result > 0 else {
                throw FlasherError.io("Serial write failed: \(String(cString: strerror(errno)))")
            }
            written += result
        }
    }

    private func readLine(timeout: TimeInterval, includeEmptyAcknowledgement: Bool = false) throws -> String {
        guard descriptor >= 0 else { throw FlasherError.io("Serial port is not open") }
        let deadline = Date().addingTimeInterval(timeout)

        while Date() < deadline {
            if let carriageReturn = receiveBuffer.firstIndex(of: 13) {
                let lineData = receiveBuffer.prefix(upTo: carriageReturn)
                receiveBuffer.removeSubrange(...carriageReturn)
                let line = String(data: lineData, encoding: .ascii) ?? ""
                if includeEmptyAcknowledgement || !line.isEmpty { return line }
            }

            var pollDescriptor = pollfd(fd: descriptor, events: Int16(POLLIN), revents: 0)
            let remaining = max(1, Int32(deadline.timeIntervalSinceNow * 1000))
            let pollResult = Darwin.poll(&pollDescriptor, 1, remaining)
            if pollResult < 0 {
                throw FlasherError.io("Serial poll failed: \(String(cString: strerror(errno)))")
            }
            if pollResult == 0 { continue }

            var bytes = [UInt8](repeating: 0, count: 256)
            let count = Darwin.read(descriptor, &bytes, bytes.count)
            if count > 0 {
                if bytes[0] == 7 { throw FlasherError.protocolError("SLCAN adapter returned BEL/error") }
                receiveBuffer.append(contentsOf: bytes.prefix(count))
            }
        }
        throw FlasherError.io("Serial read timed out")
    }
}
