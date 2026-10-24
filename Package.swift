// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SQLiteGraph",
    products: [
        .library(name: "CSQLiteGraph", targets: ["CSQLiteGraph"])
    ],
    targets: [
        .target(
            name: "CSQLiteGraph",
            path: ".",
            exclude: ["tests", "Makefile", "README.md", "LICENSE", ".github"],
            sources: ["src"],
            publicHeadersPath: "include",
            cSettings: [.define("SQLITE_CORE")]
        )
    ]
)
