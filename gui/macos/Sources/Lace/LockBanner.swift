//
// Lace macOS frontend
// LockBannerView: the bar over a tab's content while a statement the user
// asked for runs on its connection: the statement's first line, the time
// it has been running, a Cancel button. Every tab of the connection shows
// it (ContentViewController hands the connection's user statement around).
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

final class LockBannerView: NSView {
  private let spinner = NSProgressIndicator()
  private let label = NSTextField(labelWithString: "")
  private let elapsed = NSTextField(labelWithString: "")
  private let cancel = NSButton(title: "Cancel", target: nil, action: nil)
  private var timer: Timer?
  private var since = Date()
  var onCancel: (() -> Void)?

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
    spinner.style = .spinning
    spinner.controlSize = .small
    spinner.isDisplayedWhenStopped = false
    spinner.translatesAutoresizingMaskIntoConstraints = false
    label.font = .monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
    label.lineBreakMode = .byTruncatingTail
    label.translatesAutoresizingMaskIntoConstraints = false
    label.setContentHuggingPriority(.defaultLow, for: .horizontal)
    label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    elapsed.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    elapsed.textColor = .secondaryLabelColor
    elapsed.translatesAutoresizingMaskIntoConstraints = false
    cancel.bezelStyle = .rounded
    cancel.controlSize = .small
    cancel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    cancel.target = self
    cancel.action = #selector(cancelTapped)
    cancel.toolTip = "Cancel the statement (Esc)"
    cancel.translatesAutoresizingMaskIntoConstraints = false
    for v in [spinner, label, elapsed, cancel] {
      addSubview(v)
    }
    NSLayoutConstraint.activate([
      spinner.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
      spinner.centerYAnchor.constraint(equalTo: centerYAnchor),
      label.leadingAnchor.constraint(equalTo: spinner.trailingAnchor, constant: 8),
      label.centerYAnchor.constraint(equalTo: centerYAnchor),
      elapsed.leadingAnchor.constraint(equalTo: label.trailingAnchor, constant: 10),
      elapsed.centerYAnchor.constraint(equalTo: centerYAnchor),
      cancel.leadingAnchor.constraint(equalTo: elapsed.trailingAnchor, constant: 10),
      cancel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
      cancel.centerYAnchor.constraint(equalTo: centerYAnchor),
      heightAnchor.constraint(equalToConstant: 30),
    ])
    isHidden = true
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func updateLayer() {
    layer?.backgroundColor = NSColor.controlAccentColor.withAlphaComponent(0.14).cgColor
  }

  /// What the banner says; nil hides it
  var statement: Connection.UserStatement? {
    didSet {
      guard let s = statement else {
        hide()
        return
      }
      since = s.since
      var line = s.what.split(whereSeparator: \.isNewline).first.map(String.init) ?? s.what
      line = line.replacingOccurrences(of: "\t", with: " ")
      label.stringValue = line
      label.toolTip = s.what
      tick()
      if isHidden {
        isHidden = false
        spinner.startAnimation(nil)
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
          Task { @MainActor in self?.tick() }
        }
      }
    }
  }

  var text: String { "\(label.stringValue)  \(elapsed.stringValue)" }

  private func tick() {
    elapsed.stringValue = "\(Format.elapsed(Date().timeIntervalSince(since)))  ·  Esc cancels"
  }

  private func hide() {
    timer?.invalidate()
    timer = nil
    spinner.stopAnimation(nil)
    isHidden = true
  }

  @objc private func cancelTapped() {
    onCancel?()
  }
}
