// swift-tools-version: 6.1
import PackageDescription

let package = Package(
    name: "DucatiFlasher",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "ducati-flasher", targets: ["DucatiFlasher"]),
        .executable(name: "ducati-flasher-selftest", targets: ["DucatiFlasherSelfTest"]),
        .library(name: "DucatiFlasherCore", targets: ["DucatiFlasherCore"]),
    ],
    targets: [
        .target(name: "DucatiFlasherCore"),
        .executableTarget(
            name: "DucatiFlasher",
            dependencies: ["DucatiFlasherCore"]
        ),
        .executableTarget(
            name: "DucatiFlasherSelfTest",
            dependencies: ["DucatiFlasherCore"]
        ),
    ]
)
