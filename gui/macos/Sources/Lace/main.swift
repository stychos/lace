//
// Lace macOS frontend
// Entry point
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
// A scripted run (LACE_MACOS_SCRIPT) keeps out of the Dock and never takes focus
app.setActivationPolicy(ScriptDriver.isRequested ? .accessory : .regular)
app.run()
