//
// Lace macOS frontend
// TabStripView: a row of closable tabs (connections in a workspace, table
// and query views in a connection). A tab is dragged to reorder it, or
// dropped on another strip of the same drag group (another window's
// connection tabs): the owner moves its entry (onMove, onAccept). The
// drop's place is the gap nearest the pointer, marked while dragging.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit

extension NSPasteboard.PasteboardType {
  /// A tab being dragged within the app (the item is the tab's id)
  static let laceTab = NSPasteboard.PasteboardType("dev.hexed.lace.tab")
}

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
  /// More items for a tab's context menu (the table's actions)
  var menuItemsProvider: ((UUID) -> [NSMenuItem])?
  /// Strips of one group take each other's tabs (every window's connection
  /// tabs); nil: tabs reorder within this strip only
  var dragGroup: String?
  /// A tab of this strip was dropped at a new index (its final position)
  var onMove: ((UUID, Int) -> Void)?
  /// A tab of another strip of the group was dropped here, before index
  var onAccept: ((UUID, TabStripView, Int) -> Void)?

  private let scroll = NSScrollView()
  private let stack = NSStackView()
  private var buttons: [UUID: TabButton] = [:]
  /// Where a drop would land, while one is over the strip
  private let marker = NSView()

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
    marker.wantsLayer = true
    marker.layer?.backgroundColor = NSColor.controlAccentColor.cgColor
    marker.layer?.cornerRadius = 1
    marker.isHidden = true
    addSubview(marker)
    registerForDraggedTypes([.laceTab])
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
      button.strip = self
      button.configure(item)
      button.onSelect = { [weak self] in self?.onSelect?(item.id) }
      button.onClose = { [weak self] in self?.onClose?(item.id) }
      button.onCloseOthers = { [weak self] in self?.onCloseOthers?(item.id) }
      button.onCloseAll = { [weak self] in self?.onCloseAll?() }
      button.extraMenuItems = { [weak self] in self?.menuItemsProvider?(item.id) ?? [] }
      stack.addArrangedSubview(button)
      kept[item.id] = button
    }
    buttons = kept
    updateSelection()
  }

  // MARK: Dragging (the source is a TabButton, see beginDrag)

  /// The tabs' ids in strip order
  var itemIDs: [UUID] { items.map(\.id) }

  /// The gap a drop at point (in this view) lands in: 0 before the first
  /// tab ... items.count after the last
  func insertionIndex(at point: NSPoint) -> Int {
    stack.layoutSubtreeIfNeeded()
    let x = convert(point, to: stack).x
    for (i, item) in items.enumerated() {
      guard let b = buttons[item.id] else { continue }
      if x < b.frame.midX {
        return i
      }
    }
    return items.count
  }

  /// A point in this view over the gap before tab index (the script driver
  /// drops there, as the pointer would)
  func point(forGap index: Int) -> NSPoint {
    stack.layoutSubtreeIfNeeded()
    let frames = items.compactMap { buttons[$0.id]?.frame }
    let x: CGFloat
    if frames.isEmpty {
      x = 6
    } else if index <= 0 {
      x = frames[0].minX + 1
    } else if index >= frames.count {
      x = frames[frames.count - 1].maxX + 1
    } else {
      x = (frames[index - 1].maxX + frames[index].minX) / 2
    }
    return convert(NSPoint(x: x, y: bounds.midY), from: stack)
  }

  /// Whether a tab dragged from source may land here
  func accepts(from source: TabStripView) -> Bool {
    source === self || (dragGroup != nil && source.dragGroup == dragGroup)
  }

  /// A tab of source dropped at point: a move within this strip, or a tab
  /// taken from another strip of the group. False when nothing changes.
  @discardableResult
  func drop(_ id: UUID, from source: TabStripView, at point: NSPoint) -> Bool {
    guard accepts(from: source) else { return false }
    let gap = insertionIndex(at: point)
    if source === self {
      guard let from = itemIDs.firstIndex(of: id) else { return false }
      let to = gap > from ? gap - 1 : gap
      guard to != from else { return false }
      onMove?(id, to)
      return true
    }
    guard source.itemIDs.contains(id) else { return false }
    onAccept?(id, source, gap)
    return true
  }

  private func source(of info: NSDraggingInfo) -> (UUID, TabStripView)? {
    guard let button = info.draggingSource as? TabButton, let strip = button.strip, accepts(from: strip) else {
      return nil
    }
    return (button.id, strip)
  }

  private func showMarker(at location: NSPoint) {
    let gap = insertionIndex(at: location)
    let p = point(forGap: gap)
    marker.frame = NSRect(x: p.x - 1, y: 4, width: 2, height: bounds.height - 8)
    marker.isHidden = false
  }

  override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
    draggingUpdated(sender)
  }

  override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
    guard source(of: sender) != nil else {
      marker.isHidden = true
      return []
    }
    showMarker(at: convert(sender.draggingLocation, from: nil))
    return .move
  }

  override func draggingExited(_ sender: NSDraggingInfo?) {
    marker.isHidden = true
  }

  override func draggingEnded(_ sender: NSDraggingInfo) {
    marker.isHidden = true
  }

  override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
    marker.isHidden = true
    guard let (id, strip) = source(of: sender) else { return false }
    // After the drag session is over: the drop may close the source's window
    let point = convert(sender.draggingLocation, from: nil)
    DispatchQueue.main.async { [weak self] in
      self?.drop(id, from: strip, at: point)
    }
    return true
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

/// One tab: icon, title, badge, close button; dragging it moves it
final class TabButton: NSView, NSDraggingSource {
  let id: UUID
  /// The strip showing it (a drop finds the drag's source strip through it)
  weak var strip: TabStripView?
  var onSelect: (() -> Void)?
  var onClose: (() -> Void)?
  var onCloseOthers: (() -> Void)?
  var onCloseAll: (() -> Void)?
  var extraMenuItems: (() -> [NSMenuItem])?
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
  /// Where the press started (a drag begins once the pointer moves away)
  private var pressPoint: NSPoint?

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
    pressPoint = event.locationInWindow
    onSelect?()
  }

  override func mouseUp(with event: NSEvent) {
    pressPoint = nil
  }

  override func mouseDragged(with event: NSEvent) {
    guard let start = pressPoint else { return }
    let p = event.locationInWindow
    guard hypot(p.x - start.x, p.y - start.y) > 4 else { return }
    pressPoint = nil
    let item = NSPasteboardItem()
    item.setString(id.uuidString, forType: .laceTab)
    let dragging = NSDraggingItem(pasteboardWriter: item)
    dragging.setDraggingFrame(bounds, contents: snapshot())
    beginDraggingSession(with: [dragging], event: event, source: self)
  }

  /// The tab as it looks, for the drag image
  private func snapshot() -> NSImage {
    let image = NSImage(size: bounds.size)
    if let rep = bitmapImageRepForCachingDisplay(in: bounds) {
      cacheDisplay(in: bounds, to: rep)
      image.addRepresentation(rep)
    }
    return image
  }

  func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
    context == .withinApplication ? .move : []
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
    for item in extraMenuItems?() ?? [] {
      menu.addItem(item)
    }
    return menu
  }

  @objc private func closeTapped() { onClose?() }
  @objc private func closeOthersTapped() { onCloseOthers?() }
  @objc private func closeAllTapped() { onCloseAll?() }
}
