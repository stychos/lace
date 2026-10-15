//
// Lace macOS frontend
// RowModel: a sparse, paged view of a table or query result
//
// A port of the GTK frontend's row_model.c. The model's count is the whole
// table (the total once a count has answered, else what has been seen plus
// one page), so a table view scrolls the entire table natively while only a
// few pages are held: pages of pageSize rows are fetched on demand through
// the fetch callback, kept in a small LRU cache and dropped on reset (every
// sort, filter or refresh is a new generation; pages of an older generation
// are ignored when they land). A row whose page is missing shows a
// placeholder until the page arrives, then onRowsChanged names the rows to
// redraw. The model is driven from the main thread only.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation

@MainActor
public final class RowModel {
  /// The TUI's and the GTK frontend's page size
  public nonisolated static let defaultPageSize = 1000
  /// Pages kept before the least recently used one is dropped
  public nonisolated static let maxPages = 8

  public private(set) var pageSize: Int
  /// Bumped by every reset; pages loaded for an older generation are dropped
  public private(set) var generation: UInt64 = 0
  /// The columns of the first page that landed (empty until then)
  public private(set) var columns: [Column] = []
  /// A count answered (or a short page ended the table)
  public private(set) var totalKnown = false
  /// The count is a catalog estimate
  public private(set) var totalApproximate = false
  /// What the view shows: the total when known, else what has been seen
  /// plus one page to scroll into
  public private(set) var count = 0

  /// Asked for a page the view needs (once per page until it lands or fails)
  public var fetch: (@MainActor (RowModel, Int) -> Void)?
  /// The column set changed (a reset, or the first page landed)
  public var onColumnsChanged: (@MainActor () -> Void)?
  /// `count` changed: (old, new)
  public var onCountChanged: (@MainActor (Int, Int) -> Void)?
  /// The total or its qualifiers (known, approximate) changed
  public var onTotalChanged: (@MainActor () -> Void)?
  /// Cells in these rows changed (a page landed or was dropped, a cell was set)
  public var onRowsChanged: (@MainActor (Range<Int>) -> Void)?

  private struct Page {
    let result: ResultSet
    var lastUsed: UInt64
  }

  private var pages: [Int: Page] = [:]
  private var pending: Set<Int> = []
  private var useCounter: UInt64 = 0
  private var knownTotal = 0
  /// Rows actually loaded (the floor of any count)
  private var seen = 0
  /// Rows the view may scroll to while the total is unknown
  private var extent = 0

  public init(pageSize: Int = RowModel.defaultPageSize) {
    self.pageSize = max(1, pageSize)
  }

  /// The total when known, else the rows seen so far
  public var total: Int { totalKnown ? knownTotal : seen }

  /// Rows loaded (never more than the total)
  public var loadedExtent: Int { seen }

  public var loadedPageCount: Int { pages.count }

  /// Change the page size while no page is loaded (a whole result shown as
  /// one page)
  public func setPageSize(_ size: Int) {
    if size > 0 && pages.isEmpty {
      pageSize = size
    }
  }

  // MARK: Lifecycle

  /// Drop everything and start a new generation (a sort, a filter, a refresh)
  public func reset() {
    generation &+= 1
    pages.removeAll()
    pending.removeAll()
    columns = []
    knownTotal = 0
    totalKnown = false
    totalApproximate = false
    seen = 0
    extent = 0
    let old = count
    count = 0
    if old > 0 {
      onCountChanged?(old, 0)
    }
    onColumnsChanged?()
    onTotalChanged?()
  }

  /// Ask for the page holding `row` unless it is loaded or on its way
  public func request(row: Int) {
    guard row >= 0 else { return }
    let page = row / pageSize
    if pages[page] != nil || pending.contains(page) { return }
    guard let fetch else { return }
    pending.insert(page)
    fetch(self, page)
  }

  /// Mark a page as being fetched by the caller itself (the first page of a
  /// run), so the view's requests for it are not sent again
  public func notePending(page: Int) {
    if pages[page] == nil {
      pending.insert(page)
    }
  }

  public func isPageLoaded(_ page: Int) -> Bool { pages[page] != nil }
  public func isPagePending(_ page: Int) -> Bool { pending.contains(page) }

  /// A page arrived. Dropped unless it is for the current generation.
  public func pageLoaded(generation gen: UInt64, page: Int, result: ResultSet) {
    guard gen == generation else { return }
    pending.remove(page)
    if columns.isEmpty && !result.columns.isEmpty {
      columns = result.columns
      onColumnsChanged?()
    }
    useCounter += 1
    pages[page] = Page(result: result, lastUsed: useCounter)

    let start = page * pageSize
    let end = start + result.rowCount
    if end > seen {
      seen = end
    }
    if result.rowCount < pageSize {
      // A short page: the table ends here (an exact count that was only
      // waiting for this page already said so)
      knownTotal = end
      totalKnown = true
      totalApproximate = false
    } else if !totalKnown {
      // A full page while the count is still running: let the view scroll
      // one page further
      extent = max(extent, end + pageSize)
    } else if totalApproximate && end >= knownTotal {
      // An estimate that undercounted: there is more
      knownTotal = end + pageSize
    }
    if totalKnown && extent > knownTotal {
      extent = knownTotal
    }
    recomputeCount()
    evictIfNeeded()
    if start < count {
      onRowsChanged?(start..<min(max(end, start + 1), count))
    }
  }

  /// A page could not be loaded: it may be asked for again
  public func pageFailed(generation gen: UInt64, page: Int) {
    guard gen == generation else { return }
    pending.remove(page)
  }

  /// A count answered. What was seen is the floor of any estimate.
  public func setTotal(_ total: Int, known: Bool, approximate: Bool) {
    if known {
      knownTotal = max(total, seen)
      totalKnown = true
      totalApproximate = approximate
      if extent > knownTotal {
        extent = knownTotal
      }
    } else {
      totalKnown = false
      totalApproximate = false
    }
    recomputeCount()
  }

  // MARK: Cells

  /// The page result holding `row` and the row's index in it, if loaded.
  /// Marks the page recently used.
  public func rowResult(_ row: Int) -> (result: ResultSet, index: Int)? {
    guard row >= 0 else { return nil }
    let page = row / pageSize
    guard var p = pages[page] else { return nil }
    let idx = row - page * pageSize
    guard idx < p.result.rowCount else { return nil }
    useCounter += 1
    p.lastUsed = useCounter
    pages[page] = p
    return (p.result, idx)
  }

  public func isLoaded(row: Int) -> Bool { rowResult(row) != nil }

  /// nil when the row's page is not loaded (draw a placeholder)
  public func value(row: Int, column: Int) -> Value? {
    guard let (result, idx) = rowResult(row), column >= 0, column < result.columns.count else {
      return nil
    }
    return result.value(row: idx, column: column)
  }

  /// A cell was written (a verified update): the page shows the new value
  /// without a reload
  public func setCell(row: Int, column: Int, value: Value) {
    guard let (result, idx) = rowResult(row), column >= 0, column < result.columns.count else {
      return
    }
    result.replaceValue(row: idx, column: column, with: value)
    onRowsChanged?(row..<row + 1)
  }

  /// `n` rows were deleted: every page reloads (rows after them moved up)
  /// and the view shrinks by n
  public func rowsRemoved(count n: Int) {
    pages.removeAll()
    pending.removeAll()
    let n = min(max(n, 0), total)
    if totalKnown {
      knownTotal -= n
    }
    seen = max(0, seen - n)
    extent = max(0, extent - n)
    recomputeCount()
    if count > 0 {
      onRowsChanged?(0..<count)
    }
  }

  /// Drop every page (a refresh that keeps the count): rows show
  /// placeholders and re-request their pages when drawn
  public func invalidate() {
    pages.removeAll()
    pending.removeAll()
    if count > 0 {
      onRowsChanged?(0..<count)
    }
  }

  // MARK: Private

  private func recomputeCount() {
    let n = totalKnown ? knownTotal : max(extent, seen)
    let old = count
    if n != old {
      count = n
      onCountChanged?(old, n)
    }
    onTotalChanged?()
  }

  private func evictIfNeeded() {
    while pages.count > RowModel.maxPages {
      guard let oldest = pages.min(by: { $0.value.lastUsed < $1.value.lastUsed }) else { break }
      pages.removeValue(forKey: oldest.key)
      let start = oldest.key * pageSize
      if start < count {
        onRowsChanged?(start..<min(start + pageSize, count))
      }
    }
  }
}
