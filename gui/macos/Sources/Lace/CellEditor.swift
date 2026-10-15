//
// Lace macOS frontend
// The cell editor: a popover over the cell with the full edit text
//
// Return saves, Shift+Return inserts a line break, Esc cancels. NULL and
// Empty are explicit actions, because the edit rules (Value.edit) never
// turn typed text into NULL: unchanged text writes nothing, empty text is
// '', only the NULL action writes NULL.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit

final class CellEditorController: NSViewController, NSPopoverDelegate {
  /// nil is the NULL action
  var onSave: ((String?) -> Void)?
  var onCancel: (() -> Void)?

  private let popover = NSPopover()
  private let textView = EditorTextView()
  private let header: String
  private let initialText: String
  private let selectAll: Bool
  private var finished = false

  /// selectAll: the text is selected so typing replaces it (an edit started
  /// with Return); otherwise the cursor goes to the end (started by typing)
  init(column: String, typeName: String, text: String, selectAll: Bool) {
    header = "\(column) · \(typeName)"
    initialText = text
    self.selectAll = selectAll
    super.init(nibName: nil, bundle: nil)
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func loadView() {
    let root = NSView(frame: NSRect(x: 0, y: 0, width: 400, height: 170))

    let title = NSTextField(labelWithString: header)
    title.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    title.textColor = .secondaryLabelColor
    title.lineBreakMode = .byTruncatingMiddle

    let scroll = NSScrollView()
    scroll.translatesAutoresizingMaskIntoConstraints = false
    scroll.hasVerticalScroller = true
    scroll.borderType = .bezelBorder
    textView.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
    textView.isRichText = false
    textView.allowsUndo = true
    textView.isAutomaticQuoteSubstitutionEnabled = false
    textView.isAutomaticDashSubstitutionEnabled = false
    textView.isAutomaticTextReplacementEnabled = false
    textView.isAutomaticSpellingCorrectionEnabled = false
    textView.isVerticallyResizable = true
    textView.isHorizontallyResizable = false
    textView.autoresizingMask = [.width]
    textView.textContainer?.widthTracksTextView = true
    textView.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
    textView.minSize = NSSize(width: 0, height: 0)
    textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    textView.string = initialText
    textView.onCancel = { [weak self] in self?.cancelTapped() }
    scroll.documentView = textView

    let null = NSButton(title: "NULL", target: self, action: #selector(nullTapped))
    null.bezelStyle = .rounded
    null.controlSize = .small
    null.toolTip = "Write NULL"
    let empty = NSButton(title: "Empty", target: self, action: #selector(emptyTapped))
    empty.bezelStyle = .rounded
    empty.controlSize = .small
    empty.toolTip = "Write an empty string"
    let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelTapped))
    cancel.bezelStyle = .rounded
    cancel.controlSize = .small
    let save = NSButton(title: "Save", target: self, action: #selector(saveTapped))
    save.bezelStyle = .rounded
    save.controlSize = .small
    save.keyEquivalent = "\r"

    let spacer = NSView()
    spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
    let buttons = NSStackView(views: [null, empty, spacer, cancel, save])
    buttons.orientation = .horizontal
    buttons.spacing = 6

    let stack = NSStackView(views: [title, scroll, buttons])
    stack.orientation = .vertical
    stack.alignment = .leading
    stack.spacing = 8
    stack.translatesAutoresizingMaskIntoConstraints = false
    root.addSubview(stack)
    NSLayoutConstraint.activate([
      stack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 12),
      stack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -12),
      stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 10),
      stack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -10),
      title.trailingAnchor.constraint(equalTo: stack.trailingAnchor),
      scroll.leadingAnchor.constraint(equalTo: stack.leadingAnchor),
      scroll.trailingAnchor.constraint(equalTo: stack.trailingAnchor),
      scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 80),
      buttons.leadingAnchor.constraint(equalTo: stack.leadingAnchor),
      buttons.trailingAnchor.constraint(equalTo: stack.trailingAnchor),
    ])
    view = root
  }

  func show(relativeTo rect: NSRect, of view: NSView) {
    popover.contentViewController = self
    popover.behavior = .transient
    popover.delegate = self
    popover.show(relativeTo: rect, of: view, preferredEdge: .maxY)
    view.window?.makeFirstResponder(textView)
    if selectAll {
      textView.selectAll(nil)
    } else {
      textView.setSelectedRange(NSRange(location: (textView.string as NSString).length, length: 0))
    }
  }

  func popoverDidClose(_ notification: Notification) {
    if !finished {
      finished = true
      onCancel?()
    }
  }

  private func finish(_ text: String?) {
    guard !finished else { return }
    finished = true
    popover.close()
    onSave?(text)
  }

  @objc private func saveTapped() { finish(textView.string) }
  @objc private func nullTapped() { finish(nil) }
  @objc private func emptyTapped() { finish("") }

  @objc private func cancelTapped() {
    guard !finished else { return }
    finished = true
    popover.close()
    onCancel?()
  }
}

/// The editor's text view: Esc cancels (instead of word completion)
final class EditorTextView: NSTextView {
  var onCancel: (() -> Void)?

  override func cancelOperation(_ sender: Any?) {
    onCancel?()
  }

  override func complete(_ sender: Any?) {
    // Esc in a text view runs completion by default; not here
  }
}
