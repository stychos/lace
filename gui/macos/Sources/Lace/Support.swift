//
// Lace macOS frontend
// Small shared pieces: number and time formatting, alerts as async calls,
// a few plain views (text cells, the error banner, the notice bubble)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

@MainActor
enum Format {
  private static let counts: NumberFormatter = {
    let f = NumberFormatter()
    f.numberStyle = .decimal
    f.usesGroupingSeparator = true
    return f
  }()

  /// "1,234,567"
  static func count(_ n: Int) -> String {
    counts.string(from: NSNumber(value: n)) ?? String(n)
  }

  /// "3 rows", "1 row"
  static func plural(_ n: Int, _ word: String) -> String {
    "\(count(n)) \(word)\(n == 1 ? "" : "s")"
  }

  /// "12 ms", "1.4 s", "2 min 05 s"
  static func elapsed(_ seconds: Double) -> String {
    if seconds < 1 {
      return "\(Int((seconds * 1000).rounded())) ms"
    }
    if seconds < 60 {
      return String(format: "%.1f s", seconds)
    }
    let s = Int(seconds)
    return String(format: "%d min %02d s", s / 60, s % 60)
  }
}

/// Alerts as async calls: a sheet on the window when there is one, else modal
@MainActor
enum Alerts {
  /// Also told about every error shown (the script driver logs them)
  static var logger: ((String, String) -> Void)?

  static func error(_ title: String, _ message: String, window: NSWindow?) {
    logger?(title, message)
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    alert.alertStyle = .warning
    if let window {
      alert.beginSheetModal(for: window)
    } else {
      alert.runModal()
    }
  }

  /// A destructive confirmation; Cancel is the default (Return) and Esc
  static func confirm(_ title: String, _ message: String, action: String, window: NSWindow?) async
    -> Bool
  {
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    alert.alertStyle = .warning
    alert.addButton(withTitle: "Cancel")
    let go = alert.addButton(withTitle: action)
    go.hasDestructiveAction = true
    go.keyEquivalent = ""
    return await run(alert, window) == .alertSecondButtonReturn
  }

  /// One line of text; nil when cancelled
  static func prompt(
    _ title: String, _ message: String, initial: String = "", placeholder: String = "",
    secure: Bool = false, action: String = "OK", window: NSWindow?
  ) async -> String? {
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    let frame = NSRect(x: 0, y: 0, width: 320, height: 24)
    let field: NSTextField = secure ? NSSecureTextField(frame: frame) : NSTextField(frame: frame)
    field.stringValue = initial
    field.placeholderString = placeholder
    alert.accessoryView = field
    alert.addButton(withTitle: action)
    alert.addButton(withTitle: "Cancel")
    alert.window.initialFirstResponder = field
    let response = await run(alert, window)
    return response == .alertFirstButtonReturn ? field.stringValue : nil
  }

  private static func run(_ alert: NSAlert, _ window: NSWindow?) async -> NSApplication.ModalResponse {
    guard let window else { return alert.runModal() }
    return await withCheckedContinuation { cont in
      alert.beginSheetModal(for: window) { cont.resume(returning: $0) }
    }
  }
}

/// A table cell view with one label (NSTableCellView wires the text colour
/// to the row's selection state)
final class TextCellView: NSTableCellView {
  let label = NSTextField(labelWithString: "")

  override init(frame: NSRect) {
    super.init(frame: frame)
    label.translatesAutoresizingMaskIntoConstraints = false
    label.lineBreakMode = .byTruncatingTail
    label.cell?.usesSingleLineMode = true
    addSubview(label)
    textField = label
    NSLayoutConstraint.activate([
      label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
      label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
      label.centerYAnchor.constraint(equalTo: centerYAnchor),
    ])
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  /// Make or reuse one for the table view
  static func make(in tableView: NSTableView, text: String) -> TextCellView {
    let id = NSUserInterfaceItemIdentifier("TextCellView")
    let cell =
      (tableView.makeView(withIdentifier: id, owner: nil) as? TextCellView)
      ?? {
        let c = TextCellView(frame: .zero)
        c.identifier = id
        return c
      }()
    cell.label.stringValue = text
    cell.label.textColor = .labelColor
    return cell
  }
}

/// A row of text with an icon (sidebar tables)
final class IconTextCellView: NSTableCellView {
  let icon = NSImageView()
  let label = NSTextField(labelWithString: "")

  override init(frame: NSRect) {
    super.init(frame: frame)
    icon.translatesAutoresizingMaskIntoConstraints = false
    icon.imageScaling = .scaleProportionallyDown
    icon.contentTintColor = .secondaryLabelColor
    label.translatesAutoresizingMaskIntoConstraints = false
    label.lineBreakMode = .byTruncatingTail
    label.cell?.usesSingleLineMode = true
    addSubview(icon)
    addSubview(label)
    imageView = icon
    textField = label
    NSLayoutConstraint.activate([
      icon.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
      icon.centerYAnchor.constraint(equalTo: centerYAnchor),
      icon.widthAnchor.constraint(equalToConstant: 16),
      icon.heightAnchor.constraint(equalToConstant: 16),
      label.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 6),
      label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
      label.centerYAnchor.constraint(equalTo: centerYAnchor),
    ])
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  static func make(in tableView: NSTableView, symbol: String, text: String) -> IconTextCellView {
    let id = NSUserInterfaceItemIdentifier("IconTextCellView")
    let cell =
      (tableView.makeView(withIdentifier: id, owner: nil) as? IconTextCellView)
      ?? {
        let c = IconTextCellView(frame: .zero)
        c.identifier = id
        return c
      }()
    cell.icon.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
    cell.label.stringValue = text
    return cell
  }
}

/// A one-line error banner above a view's content, hidden until shown
final class BannerView: NSView {
  private let label = NSTextField(wrappingLabelWithString: "")
  private let close = NSButton()
  var onClose: (() -> Void)?

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
    label.translatesAutoresizingMaskIntoConstraints = false
    label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    label.maximumNumberOfLines = 3
    label.lineBreakMode = .byTruncatingTail
    close.translatesAutoresizingMaskIntoConstraints = false
    close.bezelStyle = .inline
    close.isBordered = false
    close.image = NSImage(systemSymbolName: "xmark.circle.fill", accessibilityDescription: "Dismiss")
    close.imagePosition = .imageOnly
    close.target = self
    close.action = #selector(closeTapped)
    close.contentTintColor = .secondaryLabelColor
    addSubview(label)
    addSubview(close)
    NSLayoutConstraint.activate([
      label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
      label.topAnchor.constraint(equalTo: topAnchor, constant: 6),
      label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
      close.leadingAnchor.constraint(equalTo: label.trailingAnchor, constant: 8),
      close.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
      close.centerYAnchor.constraint(equalTo: centerYAnchor),
      close.widthAnchor.constraint(equalToConstant: 18),
    ])
    isHidden = true
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func updateLayer() {
    layer?.backgroundColor = NSColor.systemRed.withAlphaComponent(0.12).cgColor
  }

  func show(_ text: String) {
    label.stringValue = text
    isHidden = false
  }

  func hide() {
    isHidden = true
  }

  @objc private func closeTapped() {
    hide()
    onClose?()
  }
}

/// A transient message bubble (a toast) over the content
final class NoticeView: NSVisualEffectView {
  private let label = NSTextField(labelWithString: "")
  private var hideTask: Task<Void, Never>?

  override init(frame: NSRect) {
    super.init(frame: frame)
    material = .popover
    blendingMode = .withinWindow
    state = .active
    wantsLayer = true
    layer?.cornerRadius = 8
    layer?.masksToBounds = true
    label.translatesAutoresizingMaskIntoConstraints = false
    label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    label.lineBreakMode = .byTruncatingTail
    label.maximumNumberOfLines = 2
    addSubview(label)
    NSLayoutConstraint.activate([
      label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
      label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
      label.topAnchor.constraint(equalTo: topAnchor, constant: 7),
      label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -7),
    ])
    isHidden = true
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  func show(_ text: String, for seconds: Double = 4) {
    label.stringValue = text
    isHidden = false
    hideTask?.cancel()
    hideTask = Task { [weak self] in
      try? await Task.sleep(for: .seconds(seconds))
      guard !Task.isCancelled else { return }
      self?.isHidden = true
    }
  }
}

/// A 1 pt line
final class SeparatorView: NSView {
  override var intrinsicContentSize: NSSize { NSSize(width: NSView.noIntrinsicMetric, height: 1) }

  override func draw(_ dirtyRect: NSRect) {
    NSColor.separatorColor.setFill()
    bounds.fill()
  }
}
