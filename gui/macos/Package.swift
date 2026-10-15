// swift-tools-version:6.0
//
// Lace macOS frontend
//
// LaceKit wraps liblace for Swift (CLace is liblace's headers as a Clang
// module), Lace is the app, LaceKitChecks its test program. liblace.a is
// linked by path, never with -llace, which would pick the shared library once
// `make -C liblace both` has built it. The Makefile builds liblace and laced
// first and assembles build/Lace.app; this manifest alone is enough for
// `swift build` once they exist.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import PackageDescription

let root = Context.packageDirectory + "/../.."

// liblace, and the TUI's saved-connections and history code the Makefile
// compiles into build/shared/liblaceshared.a (`make shared`), so the three
// frontends share connections.json and history/<id>.json.
// cJSON for liblace: the Makefile points LACE_CJSON_STATIC at Homebrew's
// libcjson.a so the app does not depend on /opt/homebrew at run time; a
// plain `swift build` links the shared library from the usual prefixes
var liblace: [LinkerSetting] = [
  .unsafeFlags([Context.packageDirectory + "/build/shared/liblaceshared.a"]),
  .unsafeFlags(["\(root)/liblace/build/liblace.a"]),
]
if let archive = Context.environment["LACE_CJSON_STATIC"], !archive.isEmpty {
  liblace.append(.unsafeFlags([archive]))
} else {
  liblace.append(.unsafeFlags(["-L/opt/homebrew/lib", "-L/usr/local/lib"]))
  liblace.append(.linkedLibrary("cjson"))
}

// The TUI headers in CLace.h reach liblace through "../../../liblace/..."
// paths that resolve only with the TUI's source directory on the include
// path (the TUI compiles with -Isrc), so the Clang importer gets it too
let importerFlags: [SwiftSetting] = [.unsafeFlags(["-Xcc", "-I\(root)/tui/ncurses/src"])]

let package = Package(
  name: "Lace",
  platforms: [.macOS(.v14)],
  products: [
    .executable(name: "Lace", targets: ["Lace"]),
    .library(name: "LaceKit", targets: ["LaceKit"]),
  ],
  targets: [
    .systemLibrary(name: "CLace", path: "Sources/CLace"),
    .target(
      name: "LaceKit", dependencies: ["CLace"], swiftSettings: importerFlags, linkerSettings: liblace),
    .executableTarget(name: "Lace", dependencies: ["LaceKit"], swiftSettings: importerFlags),
    .executableTarget(
      name: "LaceKitChecks", dependencies: ["LaceKit"], path: "Tests/LaceKitChecks",
      swiftSettings: importerFlags),
  ]
)
