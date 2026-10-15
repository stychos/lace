//
// Lace macOS frontend
// The welcome page a workspace shows while it has no connection: a
// connection string or file to connect to, and the saved connections
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class WelcomeViewController: NSViewController {
  var onConnect: ((String) -> Void)?
  var onOpenFile: (() -> Void)?
  var onConnectSaved: ((SavedConnection) -> Void)?
  /// Open the connect sheet, on a saved connection or a new one
  var onEdit: ((SavedConnection?) -> Void)?
  var onConnectRecent: ((String) -> Void)?
  /// Open the connect sheet with a recent URL in the form
  var onSaveRecent: ((String) -> Void)?

  private let field = NSTextField()
  private let list = SavedListView(frame: .zero)

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 800, height: 600))

    let title = NSTextField(labelWithString: "Lace")
    title.font = .systemFont(ofSize: 30, weight: .semibold)
    title.alignment = .center

    let subtitle = NSTextField(labelWithString: "Connect to a database to begin.")
    subtitle.font = .systemFont(ofSize: 14)
    subtitle.textColor = .secondaryLabelColor
    subtitle.alignment = .center

    field.placeholderString = "postgres://user@host/db, mysql://…, sqlite:///path, or a file path"
    field.font = .systemFont(ofSize: 13)
    field.target = self
    field.action = #selector(connectTapped)
    field.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let connect = NSButton(title: "Connect", target: self, action: #selector(connectTapped))
    connect.bezelStyle = .rounded
    let open = NSButton(title: "Open SQLite File…", target: self, action: #selector(openTapped))
    open.bezelStyle = .rounded
    let row = NSStackView(views: [field, connect, open])
    row.orientation = .horizontal
    row.spacing = 8

    let savedTitle = NSTextField(labelWithString: "Saved and recent connections")
    savedTitle.font = .systemFont(ofSize: 13, weight: .semibold)
    savedTitle.textColor = .secondaryLabelColor
    let manage = NSButton(title: "New Connection…", target: self, action: #selector(newTapped))
    manage.bezelStyle = .rounded
    manage.controlSize = .small
    manage.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let header = NSStackView(views: [savedTitle, spacer, manage])
    header.orientation = .horizontal

    list.translatesAutoresizingMaskIntoConstraints = false
    list.onActivate = { [weak self] c in self?.onConnectSaved?(c) }
    list.onEdit = { [weak self] c in self?.onEdit?(c) }
    list.onDuplicate = { [weak self] c in
      guard let self else { return }
      _ = SavedActions.duplicate(c, window: view.window)
      list.reload()
    }
    list.onDelete = { [weak self] c in
      Task { [weak self] in
        guard let self else { return }
        if await SavedActions.delete(c, window: view.window) {
          list.reload()
        }
      }
    }
    list.onNewConnection = { [weak self] _ in self?.onEdit?(nil) }
    list.onConnectRecent = { [weak self] url in self?.onConnectRecent?(url) }
    list.onSaveRecent = { [weak self] url in self?.onSaveRecent?(url) }
    list.onRenameFolder = { [weak self] f in
      Task { [weak self] in
        await SavedActions.renameFolder(f, window: self?.view.window)
        self?.list.reload()
      }
    }
    list.onDeleteFolder = { [weak self] f in
      SavedActions.deleteFolder(f, window: self?.view.window)
      self?.list.reload()
    }

    let hint = NSTextField(
      wrappingLabelWithString:
        "⌘T opens the connect window with the form to save a connection. ⌘O opens a SQLite file. Double-click a saved connection to connect.")
    hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    hint.textColor = .tertiaryLabelColor
    hint.alignment = .center
    hint.preferredMaxLayoutWidth = 560

    let stack = NSStackView(views: [title, subtitle, row, header, list, hint])
    stack.orientation = .vertical
    stack.alignment = .centerX
    stack.spacing = 12
    stack.setCustomSpacing(24, after: subtitle)
    stack.setCustomSpacing(28, after: row)
    stack.setCustomSpacing(4, after: header)
    stack.translatesAutoresizingMaskIntoConstraints = false
    for v in [row, header, list] {
      v.widthAnchor.constraint(equalToConstant: 600).isActive = true
    }
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.centerXAnchor.constraint(equalTo: root.centerXAnchor),
      stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 60),
      stack.bottomAnchor.constraint(lessThanOrEqualTo: root.bottomAnchor, constant: -30),
      list.heightAnchor.constraint(lessThanOrEqualToConstant: 320),
      list.heightAnchor.constraint(greaterThanOrEqualToConstant: 120),
    ])
    view = root
  }

  override func viewWillAppear() {
    super.viewWillAppear()
    list.reload()
  }

  func reload() {
    list.reload()
  }

  func focus() {
    view.window?.makeFirstResponder(field)
  }

  @objc private func connectTapped() {
    let text = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty else {
      focus()
      return
    }
    onConnect?(text)
  }

  @objc private func openTapped() {
    onOpenFile?()
  }

  @objc private func newTapped() {
    onEdit?(nil)
  }
}
