//
// Lace macOS frontend
// TabStripView: a row of closable tabs (connections in a workspace, table
// and query views in a connection)
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit

final class TabStripView: NSView {
  struct Item: Equatable {
    let id: UUID
    var title: String
    /// An SF Symbol name
    var symbol: String?
    var tooltip: String?
    /// A preview tab: drawn lighter
    var dimmed = false
    /// "TX", "TX LOST": drawn after the title
    var badge: String?
  }

  var items: [Item] = [] {
    didSet { if items != oldValue { rebuild() } }
  }
  var selectedID: UUID? {
    didSet { if selectedID != oldValue { updateSelection() } }
  }
  var onSelect: ((UUID) -> Void)?
  var onClose: ((UUID) -> Void)?
  var onCloseOthers: ((UUID) -> Void)?
  var onCloseAll: (() -> Void)?

  private let scroll = NSScrollView()
  private let stack = NSStackView()
  private var buttons: [UUID: TabButton] = [:]

  override init(frame: NSRect) {
    super.init(frame: frame)
    scroll.translatesAutoresizingMaskIntoConstraints = false
    scroll.hasHorizontalScroller = false
    scroll.hasVerticalScroller = false
    scroll.drawsBackground = false
    scroll.borderType = .noBorder
    scroll.horizontalScrollElasticity = .none
    scroll.verticalScrollElasticity = .none
    stack.orientation = .horizontal
    stack.alignment = .centerY
    stack.spacing = 2
    stack.edgeInsets = NSEdgeInsets(top: 0, left: 6, bottom: 0, right: 6)
    stack.translatesAutoresizingMaskIntoConstraints = false
    scroll.documentView = stack
    addSubview(scroll)
    NSLayoutConstraint.activate([
      // A fixed height: an intrinsic size alone loses against the holding
      // priorities of split views further down and the strip grows
      heightAnchor.constraint(equalToConstant: 30),
      scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
      scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
      scroll.topAnchor.constraint(equalTo: topAnchor),
      scroll.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -1),
      stack.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
      stack.topAnchor.constraint(equalTo: scroll.contentView.topAnchor),
      stack.heightAnchor.constraint(equalTo: scroll.contentView.heightAnchor),
    ])
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  override func draw(_ dirtyRect: NSRect) {
    NSColor.separatorColor.setFill()
    NSRect(x: 0, y: 0, width: bounds.width, height: 1).fill()
  }

  private func rebuild() {
    for v in stack.arrangedSubviews {
      stack.removeArrangedSubview(v)
      v.removeFromSuperview()
    }
    var kept: [UUID: TabButton] = [:]
    for item in items {
      let button = buttons[item.id] ?? TabButton(id: item.id)
      button.configure(item)
      button.onSelect = { [weak self] in self?.onSelect?(item.id) }
      button.onClose = { [weak self] in self?.onClose?(item.id) }
      button.onCloseOthers = { [weak self] in self?.onCloseOthers?(item.id) }
      button.onCloseAll = { [weak self] in self?.onCloseAll?() }
      stack.addArrangedSubview(button)
      kept[item.id] = button
    }
    buttons = kept
    updateSelection()
  }

  private func updateSelection() {
    for (id, button) in buttons {
      button.isSelected = id == selectedID
    }
    if let selectedID, let button = buttons[selectedID] {
      stack.layoutSubtreeIfNeeded()
      button.scrollToVisible(button.bounds)
    }
  }
}

/// One tab: icon, title, badge, close button
final class TabButton: NSView {
  let id: UUID
  var onSelect: (() -> Void)?
  var onClose: (() -> Void)?
  var onCloseOthers: (() -> Void)?
  var onCloseAll: (() -> Void)?
  var isSelected = false {
    didSet {
      if isSelected != oldValue {
        updateColors()
        needsDisplay = true
      }
    }
  }

  private let icon = NSImageView()
  private let label = NSTextField(labelWithString: "")
  private let badge = NSTextField(labelWithString: "")
  private let close = NSButton()
  private var dimmed = false
  private var hovering = false
  private var tracking: NSTrackingArea?

  init(id: UUID) {
    self.id = id
    super.init(frame: .zero)
    wantsLayer = true
    icon.translatesAutoresizingMaskIntoConstraints = false
    icon.imageScaling = .scaleProportionallyDown
    label.translatesAutoresizingMaskIntoConstraints = false
    label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
    label.lineBreakMode = .byTruncatingTail
    label.cell?.usesSingleLineMode = true
    label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    badge.translatesAutoresizingMaskIntoConstraints = false
    badge.font = .boldSystemFont(ofSize: 9)
    badge.textColor = .systemOrange
    close.translatesAutoresizingMaskIntoConstraints = false
    close.isBordered = false
    close.bezelStyle = .inline
    close.imagePosition = .imageOnly
    close.image = NSImage(systemSymbolName: "xmark", accessibilityDescription: "Close")
    close.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 9, weight: .bold)
    close.contentTintColor = .secondaryLabelColor
    close.target = self
    close.action = #selector(closeTapped)
    close.toolTip = "Close"
    addSubview(icon)
    addSubview(label)
    addSubview(badge)
    addSubview(close)
    NSLayoutConstraint.activate([
      heightAnchor.constraint(equalToConstant: 24),
      widthAnchor.constraint(lessThanOrEqualToConstant: 220),
      icon.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
      icon.centerYAnchor.constraint(equalTo: centerYAnchor),
      icon.widthAnchor.constraint(equalToConstant: 14),
      icon.heightAnchor.constraint(equalToConstant: 14),
      label.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 5),
      label.centerYAnchor.constraint(equalTo: centerYAnchor),
      badge.leadingAnchor.constraint(equalTo: label.trailingAnchor, constant: 4),
      badge.centerYAnchor.constraint(equalTo: centerYAnchor),
      close.leadingAnchor.constraint(equalTo: badge.trailingAnchor, constant: 4),
      close.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
      close.centerYAnchor.constraint(equalTo: centerYAnchor),
      close.widthAnchor.constraint(equalToConstant: 14),
      close.heightAnchor.constraint(equalToConstant: 14),
    ])
    updateColors()
  }

  required init?(coder: NSCoder) { fatalError("not used") }

  func configure(_ item: TabStripView.Item) {
    label.stringValue = item.title
    toolTip = item.tooltip ?? item.title
    icon.image = item.symbol.flatMap { NSImage(systemSymbolName: $0, accessibilityDescription: nil) }
    icon.isHidden = icon.image == nil
    badge.stringValue = item.badge ?? ""
    badge.isHidden = item.badge == nil
    dimmed = item.dimmed
    updateColors()
  }

  private func updateColors() {
    label.textColor = isSelected ? .labelColor : .secondaryLabelColor
    let base = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
    label.font = dimmed ? NSFontManager.shared.convert(base, toHaveTrait: .italicFontMask) : base
    icon.contentTintColor = isSelected ? .labelColor : .secondaryLabelColor
    close.alphaValue = isSelected || hovering ? 1 : 0.35
  }

  override func updateLayer() {
    layer?.cornerRadius = 5
    if isSelected {
      layer?.backgroundColor = NSColor.labelColor.withAlphaComponent(0.1).cgColor
    } else if hovering {
      layer?.backgroundColor = NSColor.labelColor.withAlphaComponent(0.04).cgColor
    } else {
      layer?.backgroundColor = nil
    }
  }

  override func updateTrackingAreas() {
    super.updateTrackingAreas()
    if let tracking {
      removeTrackingArea(tracking)
    }
    let area = NSTrackingArea(
      rect: bounds, options: [.mouseEnteredAndExited, .activeInKeyWindow], owner: self, userInfo: nil)
    addTrackingArea(area)
    tracking = area
  }

  override func mouseEntered(with event: NSEvent) {
    hovering = true
    updateColors()
    needsDisplay = true
  }

  override func mouseExited(with event: NSEvent) {
    hovering = false
    updateColors()
    needsDisplay = true
  }

  override func mouseDown(with event: NSEvent) {
    onSelect?()
  }

  override func otherMouseDown(with event: NSEvent) {
    if event.buttonNumber == 2 {
      onClose?()
    }
  }

  override func menu(for event: NSEvent) -> NSMenu? {
    let menu = NSMenu()
    menu.addItem(withTitle: "Close Tab", action: #selector(closeTapped), keyEquivalent: "").target = self
    menu.addItem(withTitle: "Close Other Tabs", action: #selector(closeOthersTapped), keyEquivalent: "")
      .target = self
    menu.addItem(withTitle: "Close All Tabs", action: #selector(closeAllTapped), keyEquivalent: "").target =
      self
    return menu
  }

  @objc private func closeTapped() { onClose?() }
  @objc private func closeOthersTapped() { onCloseOthers?() }
  @objc private func closeAllTapped() { onCloseAll?() }
}
