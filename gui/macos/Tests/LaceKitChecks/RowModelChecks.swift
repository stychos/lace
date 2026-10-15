//
// Lace macOS frontend
// Checks: the paged row model (no daemon): requests, pages landing, the
// count and its floor, short pages, eviction, cells, deletes, generations
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import Foundation
import LaceKit

@MainActor
private func page(_ from: Int, _ n: Int) -> ResultSet {
  ResultSet(
    columns: [Column(name: "id", type: .int, primaryKey: true), Column(name: "name", type: .text)],
    rows: (0..<n).map { [.int(Int64(from + $0)), .text("row \(from + $0)")] })
}

@MainActor
func checkRowModel() {
  let m = RowModel(pageSize: 10)
  var fetched: [Int] = []
  var countChanges: [(Int, Int)] = []
  var changedRows: [Range<Int>] = []
  var columnsChanges = 0
  m.fetch = { _, p in fetched.append(p) }
  m.onCountChanged = { countChanges.append(($0, $1)) }
  m.onRowsChanged = { changedRows.append($0) }
  m.onColumnsChanged = { columnsChanges += 1 }

  Checks.equal(m.count, 0)
  Checks.equal(m.pageSize, 10)
  Checks.expect(!m.totalKnown, "nothing known at first")

  // A request goes out once per page until it lands
  m.request(row: 3)
  m.request(row: 7)
  Checks.equal(fetched, [0])
  Checks.expect(m.isPagePending(0) && !m.isPageLoaded(0), "page 0 pending")
  Checks.equal(m.value(row: 3, column: 0), nil, "a missing page has no values")

  // A full page while the count is unknown: one page further to scroll to
  let gen = m.generation
  m.pageLoaded(generation: gen, page: 0, result: page(0, 10))
  Checks.equal(m.columns.map(\.name), ["id", "name"])
  Checks.equal(columnsChanges, 1)
  Checks.equal(m.count, 20, "one page beyond what was seen")
  Checks.equal(m.loadedExtent, 10)
  Checks.equal(m.total, 10, "the total shown is what was seen")
  Checks.expect(!m.totalKnown, "still counting")
  Checks.equal(m.value(row: 3, column: 1), .text("row 3"))
  Checks.equal(m.value(row: 12, column: 1), nil)
  Checks.equal(changedRows.last, 0..<10)

  // The count answers below the scroll extent but above what was seen: it
  // wins (the extent was only room to scroll into)
  m.setTotal(15, known: true, approximate: false)
  Checks.equal(m.count, 15)
  Checks.expect(m.totalKnown && !m.totalApproximate, "exact total")
  Checks.equal(countChanges.last.map { $0.1 }, 15)

  // A short page ends the table where it ends, whatever the count said
  m.request(row: 12)
  Checks.equal(fetched, [0, 1])
  m.pageLoaded(generation: gen, page: 1, result: page(10, 3))
  Checks.equal(m.count, 13)
  Checks.equal(m.total, 13)
  Checks.equal(m.value(row: 12, column: 0), .int(12))

  // A page of an older generation is dropped on arrival
  m.reset()
  Checks.equal(m.count, 0)
  Checks.equal(m.columns.count, 0)
  Checks.expect(m.generation != gen, "reset is a new generation")
  m.pageLoaded(generation: gen, page: 0, result: page(0, 10))
  Checks.expect(!m.isPageLoaded(0), "a stale page is ignored")

  // An estimate below what was seen is raised to the extent, and an
  // estimate reached by a full page grows
  let g2 = m.generation
  m.pageLoaded(generation: g2, page: 0, result: page(0, 10))
  m.pageLoaded(generation: g2, page: 50, result: page(500, 10))
  m.setTotal(100, known: true, approximate: true)
  Checks.equal(m.count, 510, "an estimate never hides rows that were seen")
  Checks.expect(m.totalApproximate, "still an estimate")
  m.pageLoaded(generation: g2, page: 51, result: page(510, 10))
  Checks.equal(m.count, 530, "a full page at an estimate's end extends it")

  // A failed page may be asked for again
  m.request(row: 25)
  Checks.expect(m.isPagePending(2), "page 2 pending")
  m.pageFailed(generation: g2, page: 2)
  Checks.expect(!m.isPagePending(2), "failed: no longer pending")
  fetched.removeAll()
  m.request(row: 25)
  Checks.equal(fetched, [2], "asked again")
  m.notePending(page: 3)
  fetched.removeAll()
  m.request(row: 30)
  Checks.equal(fetched, [], "a page noted pending is not fetched again")

  // Eviction keeps the most recently used pages
  m.reset()
  let g3 = m.generation
  for p in 0..<RowModel.maxPages {
    m.pageLoaded(generation: g3, page: p, result: page(p * 10, 10))
  }
  Checks.equal(m.loadedPageCount, RowModel.maxPages)
  _ = m.value(row: 0, column: 0)  // page 0 is now the most recently used
  changedRows.removeAll()
  m.pageLoaded(generation: g3, page: 40, result: page(400, 10))
  Checks.equal(m.loadedPageCount, RowModel.maxPages)
  Checks.expect(m.isPageLoaded(0), "a used page stays")
  Checks.expect(!m.isPageLoaded(1), "the least recently used page went")
  Checks.expect(changedRows.contains(10..<20), "the dropped page's rows redraw")

  // A written cell shows at once; a delete shrinks the view and reloads
  m.setCell(row: 2, column: 1, value: .text("edited"))
  Checks.equal(m.value(row: 2, column: 1), .text("edited"))
  Checks.equal(m.rowResult(2).map { $0.result.value(row: $0.index, column: 1) }, .text("edited"))
  m.setTotal(410, known: true, approximate: false)
  m.rowsRemoved(count: 2)
  Checks.equal(m.count, 408)
  Checks.equal(m.loadedPageCount, 0, "every page reloads after a delete")
  Checks.expect(m.totalKnown, "the total stays known")

  // invalidate drops pages but keeps the count
  m.pageLoaded(generation: m.generation, page: 0, result: page(0, 10))
  m.invalidate()
  Checks.equal(m.count, 408)
  Checks.equal(m.loadedPageCount, 0)

  // A whole result as one page
  let whole = RowModel(pageSize: 1000)
  whole.setPageSize(3)
  whole.pageLoaded(generation: whole.generation, page: 0, result: page(0, 3))
  whole.setTotal(3, known: true, approximate: false)
  Checks.equal(whole.count, 3)
  Checks.equal(whole.pageSize, 3)
  whole.setPageSize(50)
  Checks.equal(whole.pageSize, 3, "the page size is fixed once a page is loaded")
}
