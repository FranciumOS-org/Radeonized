// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Radeonize",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "Radeonize",
            path: "Sources/Radeonize",
            resources: [.copy("Resources/radeonize-helper.sh")]
        )
    ]
)
