//
// Lace macOS frontend
// The application: the menu bar and the first workspace
//
// Every command is a menu item with a key equivalent whose action travels
// the responder chain (grid → table or query view → connection page →
// workspace → window), so the same selector reaches whichever view is
// focused, and items nothing responds to are disabled.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationDidFinishLaunching(_ notification: Notification) {
    NSApp.mainMenu = makeMainMenu()
    let first = LaceApp.shared.newWorkspace()
    if ScriptDriver.isRequested {
      ScriptDriver.startIfRequested(in: first)
    } else if LaceApp.shared.prefs.restoreSession, !Session.disabledByArguments {
      Task { await Session.restore(into: first) }
    }
  }

  /// general.on_last_workspace_close: "quit" ends the app with its last
  /// window ("new" opens another one, see LaceApp.lastWorkspaceClosed)
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    let app = LaceApp.shared
    return app.prefs.lastWorkspaceClose == .quit && !app.isTerminating && app.workspaces.isEmpty
  }

  func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
    if !flag {
      LaceApp.shared.newWorkspace()
    }
    return true
  }

  func application(_ sender: NSApplication, openFile filename: String) -> Bool {
    let workspace = LaceApp.shared.frontWorkspace ?? LaceApp.shared.newWorkspace()
    workspace.workspace.connect(to: filename)
    return true
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    LaceApp.shared.isTerminating = true
    return .terminateNow
  }

  /// AppKit closes the windows after this, so the session is written here,
  /// with every window, and the closes that follow leave it alone. With no
  /// window open the file keeps the last window's session (see
  /// windowWillClose).
  func applicationWillTerminate(_ notification: Notification) {
    if !LaceApp.shared.workspaces.isEmpty {
      Session.save()
    }
    for workspace in LaceApp.shared.workspaces {
      workspace.workspace.closeAllConnections()
    }
    // A scripted run the app ends by itself (the last window closed) says
    // whether its expectations held
    if ScriptDriver.isRequested && ScriptDriver.failureCount > 0 {
      FileHandle.standardError.write(Data("script: \(ScriptDriver.failureCount) failed (the app quit)\n".utf8))
      exit(1)
    }
  }

  @objc func newWorkspace(_ sender: Any?) {
    LaceApp.shared.newWorkspace()
  }

  @objc func showPreferences(_ sender: Any?) {
    LaceApp.shared.showPreferences()
  }

  // MARK: Menu

  private func makeMainMenu() -> NSMenu {
    let main = NSMenu()

    let app = NSMenu()
    app.addItem(
      withTitle: "About Lace", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
      keyEquivalent: "")
    app.addItem(.separator())
    app.addItem(withTitle: "Settings…", action: #selector(showPreferences(_:)), keyEquivalent: ",")
    app.addItem(.separator())
    app.addItem(withTitle: "Hide Lace", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
    let hideOthers = app.addItem(
      withTitle: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h")
    hideOthers.keyEquivalentModifierMask = [.command, .option]
    app.addItem(withTitle: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
    app.addItem(.separator())
    app.addItem(withTitle: "Quit Lace", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    main.addItem(submenu(app, title: "Lace"))

    let file = NSMenu(title: "File")
    file.addItem(withTitle: "New Workspace", action: #selector(newWorkspace(_:)), keyEquivalent: "n")
    file.addItem(
      withTitle: "New Connection Tab", action: #selector(WorkspaceViewController.newConnectionTab(_:)),
      keyEquivalent: "t")
    file.addItem(
      withTitle: "Open SQLite File…", action: #selector(WorkspaceViewController.openSQLiteFile(_:)),
      keyEquivalent: "o")
    file.addItem(.separator())
    file.addItem(withTitle: "New Query", action: #selector(WorkspaceViewController.newQuery(_:)), keyEquivalent: "e")
    file.addItem(.separator())
    file.addItem(
      withTitle: "Rename Workspace…", action: #selector(WorkspaceWindowController.renameWorkspace(_:)),
      keyEquivalent: "R")
    file.addItem(.separator())
    file.addItem(withTitle: "Close Tab", action: #selector(WorkspaceViewController.closeTab(_:)), keyEquivalent: "w")
    file.addItem(
      withTitle: "Close Connection Tab", action: #selector(WorkspaceViewController.closeConnection(_:)),
      keyEquivalent: "W")
    let closeWindow = file.addItem(
      withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
    closeWindow.keyEquivalentModifierMask = [.command, .option]
    main.addItem(submenu(file, title: "File"))

    let edit = NSMenu(title: "Edit")
    edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
    edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
    edit.addItem(.separator())
    edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
    edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    edit.addItem(.separator())
    edit.addItem(withTitle: "Add Row…", action: #selector(TableViewController.addRow(_:)), keyEquivalent: "N")
    let deleteRows = edit.addItem(
      withTitle: "Delete Rows", action: #selector(TableViewController.deleteRows(_:)),
      keyEquivalent: String(UnicodeScalar(NSBackspaceCharacter)!))
    deleteRows.keyEquivalentModifierMask = [.command]
    edit.addItem(.separator())
    let find = NSMenu(title: "Find")
    let findItem = find.addItem(
      withTitle: "Find…", action: #selector(NSTextView.performFindPanelAction(_:)), keyEquivalent: "f")
    findItem.tag = Int(NSFindPanelAction.showFindPanel.rawValue)
    findItem.keyEquivalentModifierMask = [.command, .option]
    let findNext = find.addItem(
      withTitle: "Find Next", action: #selector(NSTextView.performFindPanelAction(_:)), keyEquivalent: "g")
    findNext.tag = Int(NSFindPanelAction.next.rawValue)
    let findPrev = find.addItem(
      withTitle: "Find Previous", action: #selector(NSTextView.performFindPanelAction(_:)), keyEquivalent: "G")
    findPrev.tag = Int(NSFindPanelAction.previous.rawValue)
    edit.addItem(submenu(find, title: "Find"))
    main.addItem(submenu(edit, title: "Edit"))

    let view = NSMenu(title: "View")
    let sidebar = view.addItem(
      withTitle: "Toggle Sidebar", action: #selector(NSSplitViewController.toggleSidebar(_:)), keyEquivalent: "s")
    sidebar.keyEquivalentModifierMask = [.command, .control]
    view.addItem(
      withTitle: "Focus Sidebar", action: #selector(ConnectionViewController.focusSidebar(_:)), keyEquivalent: "1")
    view.addItem(.separator())
    view.addItem(withTitle: "Filter", action: #selector(TableViewController.toggleFilters(_:)), keyEquivalent: "f")
    view.addItem(withTitle: "Refresh", action: #selector(TableViewController.refresh(_:)), keyEquivalent: "r")
    view.addItem(withTitle: "Go to Row…", action: #selector(TableViewController.gotoRow(_:)), keyEquivalent: "l")
    view.addItem(.separator())
    // The sub-views of a table tab (⌃⌘1..5 as the TUIs' a s y f n), and following keys
    for (title, sel, key) in [
      ("Data", #selector(TableViewController.showData(_:)), "1"),
      ("Structure", #selector(TableViewController.showStructure(_:)), "2"),
      ("Indexes", #selector(TableViewController.showIndexes(_:)), "3"),
      ("Foreign Keys", #selector(TableViewController.showForeignKeys(_:)), "4"),
      ("Information", #selector(TableViewController.showInformation(_:)), "5"),
    ] as [(String, Selector, String)] {
      let item = view.addItem(withTitle: title, action: sel, keyEquivalent: key)
      item.keyEquivalentModifierMask = [.command, .control]
    }
    view.addItem(.separator())
    let follow = view.addItem(
      withTitle: "Follow Foreign Key", action: #selector(TableViewController.followForeignKey(_:)), keyEquivalent: "\r")
    follow.keyEquivalentModifierMask = [.command, .option]
    let refs = view.addItem(
      withTitle: "Referenced By…", action: #selector(TableViewController.referencedBy(_:)), keyEquivalent: "\r")
    refs.keyEquivalentModifierMask = [.command, .option, .shift]
    view.addItem(.separator())
    let nextTab = view.addItem(
      withTitle: "Next Tab", action: #selector(ContentViewController.selectNextTab(_:)), keyEquivalent: "\t")
    nextTab.keyEquivalentModifierMask = [.control]
    let prevTab = view.addItem(
      withTitle: "Previous Tab", action: #selector(ContentViewController.selectPreviousTab(_:)), keyEquivalent: "\t")
    prevTab.keyEquivalentModifierMask = [.control, .shift]
    view.addItem(
      withTitle: "Next Connection", action: #selector(WorkspaceViewController.nextConnection(_:)), keyEquivalent: "}")
    view.addItem(
      withTitle: "Previous Connection", action: #selector(WorkspaceViewController.previousConnection(_:)),
      keyEquivalent: "{")
    main.addItem(submenu(view, title: "View"))

    // The table's actions and the editors of the Structure, Indexes, Foreign
    // Keys and Information sub-views (⌥⌘ keys; Return edits, Delete drops in a list)
    let table = NSMenu(title: "Table")
    table.addItem(withTitle: "Rename Table…", action: #selector(TableViewController.renameTable(_:)), keyEquivalent: "")
    table.addItem(withTitle: "Truncate Table…", action: #selector(TableViewController.truncateTable(_:)), keyEquivalent: "")
    table.addItem(withTitle: "Drop Table…", action: #selector(TableViewController.dropTable(_:)), keyEquivalent: "")
    table.addItem(.separator())
    table.addItem(withTitle: "New Table…", action: #selector(TableViewController.newTable(_:)), keyEquivalent: "")
    table.addItem(withTitle: "New View…", action: #selector(TableViewController.newView(_:)), keyEquivalent: "")
    table.addItem(withTitle: "New Function…", action: #selector(TableViewController.newFunction(_:)), keyEquivalent: "")
    table.addItem(withTitle: "Edit in a Query View", action: #selector(TableViewController.editInQuery(_:)), keyEquivalent: "")
    table.addItem(.separator())
    for (title, sel, key) in [
      ("Add…", #selector(TableViewController.structureAdd(_:)), "="),
      ("Edit…", #selector(TableViewController.structureEdit(_:)), "e"),
      ("Drop…", #selector(TableViewController.structureDrop(_:)), "-"),
      ("Rename…", #selector(TableViewController.structureRename(_:)), "r"),
      ("Primary Key…", #selector(TableViewController.structurePrimaryKey(_:)), "p"),
      ("Move Column…", #selector(TableViewController.structureMove(_:)), "m"),
      ("Add Check Constraint…", #selector(TableViewController.structureCheck(_:)), "k"),
    ] as [(String, Selector, String)] {
      let item = table.addItem(withTitle: title, action: sel, keyEquivalent: key)
      item.keyEquivalentModifierMask = [.command, .option]
    }
    main.addItem(submenu(table, title: "Table"))

    let query = NSMenu(title: "Query")
    query.addItem(withTitle: "Run", action: #selector(QueryViewController.run(_:)), keyEquivalent: "\r")
    let runAll = query.addItem(withTitle: "Run All", action: #selector(QueryViewController.runAll(_:)), keyEquivalent: "\r")
    runAll.keyEquivalentModifierMask = [.command, .shift]
    query.addItem(
      withTitle: "Run in Transaction", action: #selector(QueryViewController.runInTransaction(_:)), keyEquivalent: "T")
    query.addItem(withTitle: "Stop", action: #selector(QueryViewController.stop(_:)), keyEquivalent: ".")
    query.addItem(.separator())
    query.addItem(
      withTitle: "Query History…", action: #selector(ConnectionViewController.showHistory(_:)), keyEquivalent: "y")
    main.addItem(submenu(query, title: "Query"))

    let window = NSMenu(title: "Window")
    window.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
    window.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
    window.addItem(.separator())
    window.addItem(
      withTitle: "Show Previous Tab", action: #selector(NSWindow.selectPreviousTab(_:)), keyEquivalent: "")
    window.addItem(withTitle: "Show Next Tab", action: #selector(NSWindow.selectNextTab(_:)), keyEquivalent: "")
    window.addItem(
      withTitle: "Merge All Windows", action: #selector(NSWindow.mergeAllWindows(_:)), keyEquivalent: "")
    window.addItem(.separator())
    window.addItem(
      withTitle: "Bring All to Front", action: #selector(NSApplication.arrangeInFront(_:)), keyEquivalent: "")
    main.addItem(submenu(window, title: "Window"))
    NSApp.windowsMenu = window

    return main
  }

  private func submenu(_ menu: NSMenu, title: String) -> NSMenuItem {
    let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
    item.submenu = menu
    return item
  }
}
