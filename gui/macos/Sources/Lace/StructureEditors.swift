//
// Lace macOS frontend
// The structure editors of a table tab: the column editor, drop, rename,
// the primary-key editor and move (Structure); create, check, drop and
// rename (Indexes); create, edit, drop and rename (Foreign keys); the
// counter, comment, engine, charset and maintenance actions (Information).
// Every change builds its statement with LaceKit's Ddl and goes through
// DdlRunner.perform; what the driver cannot do is refused before any
// confirmation, with the reason.
//
// (c) iloveyou, 2025. MIT License.
// https://github.com/stychos/lace

import AppKit
import LaceKit

/// The actions the editors' keys and menus name
enum StructureAction: CaseIterable {
  case add, edit, drop, rename, primaryKey, move, check

  var title: String {
    switch self {
    case .add: "Add…"
    case .edit: "Edit…"
    case .drop: "Drop…"
    case .rename: "Rename…"
    case .primaryKey: "Primary Key…"
    case .move: "Move Column…"
    case .check: "Add Check Constraint…"
    }
  }
}

/// The Information list's action rows (this frontend's tags, above the shared ones)
enum InfoTag {
  static let autoIncrement = 90001
  static let comment = 90002
  static let engine = 90003
  static let charset = 90004
  static let analyze = 90005
  static let vacuum = 90006
  static let optimize = 90007
  static let editDefinition = 90008
}

private let sqliteWhy = "SQLite cannot alter this; create a new table and copy the rows"
private let sqliteFKWhy = "SQLite cannot add or drop a foreign key on an existing table; create a new table and copy the rows"
private let fkActions = ["NO ACTION", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"]

extension TableViewController {
  private var driver: Driver { connection.driver }
  private var isMySQL: Bool { driver == .mysql || driver == .mariadb }

  /// "orders (12,400 rows)" from what the model knows; "orders" else
  var rowsText: String {
    guard SubView.available(for: object.kind).contains(.data), model.totalKnown, filterBar.isEmpty else { return table }
    return "\(table) (\(model.totalApproximate ? "≈" : "")\(Format.plural(model.total, "row")))"
  }

  /// The actions that mean something in the current sub-view
  func availableStructureActions() -> [StructureAction] {
    switch subview {
    case .structure where object.kind == .table: [.add, .edit, .drop, .rename, .primaryKey, .move]
    case .structure: [.edit]
    case .indexes: [.add, .check, .drop, .rename]
    case .foreignKeys: [.add, .edit, .drop, .rename]
    case .information: [.edit]
    case .data: []
    }
  }

  /// Run one on the list's selected row
  func runStructureAction(_ a: StructureAction) {
    let tag = list.selectedTag ?? CatalogTable.Tag.none
    switch subview {
    case .structure where object.kind == .table: structureAction(a, tag: tag)
    case .structure: definitionAction(a)
    case .indexes: indexesAction(a, tag: tag)
    case .foreignKeys: foreignKeysAction(a, tag: tag)
    case .information: infoAction(a, tag: tag)
    case .data: toast?("The editors live in Structure, Indexes, Foreign Keys and Information")
    }
  }

  /// A menu of the current list's actions (the strip's button, the list's context menu)
  func structureMenu() -> NSMenu {
    let menu = NSMenu()
    for a in availableStructureActions() {
      let item = menu.addItem(withTitle: a.title, action: #selector(structureMenuAction(_:)), keyEquivalent: "")
      item.target = self
      item.representedObject = StructureActionBox(a)
    }
    if !menu.items.isEmpty {
      menu.addItem(.separator())
    }
    for item in tableActionsMenu().items {
      menu.addItem(item.copy() as! NSMenuItem)
    }
    return menu
  }

  @objc func structureMenuAction(_ sender: NSMenuItem) {
    guard let box = sender.representedObject as? StructureActionBox else { return }
    runStructureAction(box.action)
  }

  @objc func structureAdd(_ sender: Any?) { runStructureAction(.add) }
  @objc func structureEdit(_ sender: Any?) { runStructureAction(.edit) }
  @objc func structureDrop(_ sender: Any?) { runStructureAction(.drop) }
  @objc func structureRename(_ sender: Any?) { runStructureAction(.rename) }
  @objc func structurePrimaryKey(_ sender: Any?) { runStructureAction(.primaryKey) }
  @objc func structureMove(_ sender: Any?) { runStructureAction(.move) }
  @objc func structureCheck(_ sender: Any?) { runStructureAction(.check) }

  // MARK: Structure

  private func column(tag: Int) -> Column? {
    guard let schema, tag >= CatalogTable.Tag.column else { return nil }
    let i = tag - CatalogTable.Tag.column
    return i < schema.columns.count ? schema.columns[i] : nil
  }

  private func structureAction(_ a: StructureAction, tag: Int) {
    guard schema != nil else {
      toast?("The structure is still loading")
      return
    }
    let c = column(tag: tag)
    switch a {
    case .add: openColumnEditor(editing: nil)
    case .edit: if let c { openColumnEditor(editing: c) } else { toast?("Pick a column first") }
    case .drop: if let c { dropColumn(c) } else { toast?("Pick a column first") }
    case .rename: if let c { renameColumn(c) } else { toast?("Pick a column first") }
    case .move: if let c { moveColumn(c) } else { toast?("Pick a column first") }
    case .primaryKey: openPrimaryKeyEditor()
    case .check: toast?("Check constraints are added from Indexes")
    }
  }

  /// The column form; editing nil adds one
  func openColumnEditor(editing cur: Column?) {
    guard let schema else { return }
    let sheet = FormSheetController(
      title: cur.map { "Column \($0.name)" } ?? "Add column to \(table)", submitTitle: cur == nil ? "Add" : "Change")
    sheet.addText(id: "name", label: "Name", value: cur?.name)
    sheet.addPick(id: "type", label: "Type", value: cur?.typeName, freeText: true)
    sheet.addToggle(id: "nullable", label: "Nullable", on: cur?.nullable ?? true)
    sheet.addText(id: "default", label: "Default", value: cur?.defaultValue, hint: "SQL: 'text', 0, CURRENT_TIMESTAMP")
    sheet.addPick(id: "collation", label: "Collation", value: cur?.collation, freeText: true)
    sheet.addText(id: "comment", label: "Comment", value: cur?.comment)
    sheet.addToggle(id: "autoinc", label: "Auto-increment", on: cur.map { $0.autoIncrement || $0.identity != 0 } ?? false)
    if isMySQL {
      let others = schema.columns.map(\.name).filter { $0 != cur?.name }
      sheet.addPick(id: "after", label: cur == nil ? "After" : "Move after", options: ["(first)"] + others, freeText: false)
    }
    if driver == .sqlite {
      sheet.disable(id: "comment", reason: "SQLite has no column comments")
      if cur != nil {
        for id in ["type", "nullable", "default", "collation", "autoinc"] {
          sheet.disable(id: id, reason: sqliteWhy)
        }
      } else {
        sheet.disable(id: "autoinc", reason: "SQLite: only INTEGER PRIMARY KEY AUTOINCREMENT at CREATE TABLE")
      }
    } else if cur?.generated == true {
      sheet.disable(id: "type", reason: "A generated column: drop it and add it again")
      sheet.disable(id: "default", reason: "A generated column: drop it and add it again")
    }
    let editing = cur?.name
    sheet.onSubmit = { [weak self] f in self?.submitColumn(f, editing: editing) }
    presentAsSheet(sheet)
    let connection = self.connection
    Task { [weak sheet] in
      if let types = try? await connection.listTypes() { sheet?.setOptions(id: "type", types) }
      if let colls = try? await connection.listCollations() { sheet?.setOptions(id: "collation", colls) }
    }
  }

  private func submitColumn(_ f: FormSheetController, editing: String?) {
    guard let schema else {
      f.showError("The table is gone")
      return
    }
    let name = f.text("name")
    guard !name.isEmpty else {
      f.showError("A name is needed")
      return
    }
    var after: String? = nil
    if isMySQL, let a = f.value("after") {
      after = a == "(first)" ? "" : a
    }
    var w = DdlColumn(
      name: name, type: f.value("type"), nullable: f.on("nullable"), defaultSQL: f.value("default"),
      collation: f.value("collation"), comment: driver == .sqlite ? nil : f.text("comment"),
      autoIncrement: f.on("autoinc"), after: after)
    let action: DdlAction
    do {
      if let editing {
        guard let cur = schema.column(named: editing) else {
          f.showError("The column is gone (the structure was reloaded)")
          return
        }
        w.name = cur.name
        if w.comment == (cur.comment ?? "") { w.comment = nil }  // unchanged
        var alter: String?
        do {
          alter = try Ddl.alterColumn(driver, table: table, schema: schema, column: cur.name, wanted: w)
        } catch let e as DdlRefused where e.reason == "No change" {
          alter = nil
        }
        let rename = name != cur.name ? try Ddl.renameColumn(driver, table: table, column: cur.name, to: name) : nil
        guard alter != nil || rename != nil else {
          f.showError("No change")
          return
        }
        let sql = [alter, rename].compactMap { $0 }.joined(separator: ";\n")  // the alter names the old column
        action = DdlAction(
          title: "Change column",
          summary: rename != nil
            ? "Change column \(cur.name) of \(table) and rename it to \(name)" : "Change column \(cur.name) of \(table)",
          sql: sql)
      } else {
        if w.comment?.isEmpty == true { w.comment = nil }
        let sql = try Ddl.addColumn(driver, table: table, column: w)
        action = DdlAction(title: "Add column", summary: "Add column \(name) to \(table)", sql: sql)
      }
    } catch {
      DdlRunner.refused(error, in: self, sheet: f)
      return
    }
    Task { await DdlRunner.perform(action, in: self, sheet: f) }
  }

  /// "index a, foreign key b": what the schema says uses the column
  private func uses(of column: String) -> String {
    guard let schema else { return "" }
    var parts: [String] = []
    for ix in schema.indexes where ix.columns.contains(column) {
      let word = ix.kind == .primary ? "the primary key" : ix.kind == .check ? "check" : "index"
      parts.append("\(word) \(ix.name ?? "")")
    }
    for fk in schema.foreignKeys where fk.columns.contains(column) {
      parts.append("foreign key \(fk.name ?? "")")
    }
    return parts.joined(separator: ", ")
  }

  private func dropColumn(_ c: Column) {
    do {
      let sql = try Ddl.dropColumn(driver, table: table, column: c.name)
      let used = uses(of: c.name)
      let summary =
        used.isEmpty
        ? "Drop column \(c.name) from \(rowsText)."
        : "Drop column \(c.name) from \(rowsText).\nIt is part of \(used): PostgreSQL drops those with it, MySQL and SQLite may refuse."
      Task { await DdlRunner.perform(DdlAction(title: "Drop column", summary: summary, sql: sql, destructive: true), in: self) }
    } catch {
      DdlRunner.refused(error, in: self)
    }
  }

  private func renameColumn(_ c: Column) {
    Task { [weak self] in
      guard let self else { return }
      guard let name = await promptName("Rename column \(c.name)", "New name", initial: c.name), name != c.name else { return }
      do {
        let sql = try Ddl.renameColumn(driver, table: table, column: c.name, to: name)
        await DdlRunner.perform(
          DdlAction(title: "Rename column", summary: "Rename column \(c.name) of \(table) to \(name)", sql: sql), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  /// The one-line prompt scripts can answer (ScriptDriver.promptAnswer)
  func promptName(_ title: String, _ label: String, initial: String? = nil) async -> String? {
    if let scripted = ScriptDriver.takePromptAnswer() {
      return scripted
    }
    guard let text = await Alerts.prompt(title, label, initial: initial ?? "", window: view.window) else { return nil }
    let name = text.trimmingCharacters(in: .whitespaces)
    return name.isEmpty ? nil : name
  }

  private func moveColumn(_ c: Column) {
    guard isMySQL, let schema else {
      toast?("Only MySQL can reorder columns; the others keep the creation order")
      return
    }
    let sheet = FormSheetController(title: "Move column \(c.name)", submitTitle: "Move")
    let others = schema.columns.map(\.name).filter { $0 != c.name }
    sheet.addPick(id: "after", label: "Move after", options: ["(first)"] + others, freeText: false)
    sheet.onSubmit = { [weak self] f in
      guard let self, let schema = self.schema, let cur = schema.column(named: c.name) else { return }
      guard let pick = f.value("after") else {
        f.showError("Pick a position")
        return
      }
      let after = pick == "(first)" ? "" : pick
      let w = DdlColumn(
        name: cur.name, type: cur.typeName, nullable: cur.nullable, defaultSQL: cur.defaultValue, collation: cur.collation,
        autoIncrement: cur.autoIncrement || cur.identity != 0, after: after)
      do {
        let sql = try Ddl.alterColumn(driver, table: table, schema: schema, column: cur.name, wanted: w)
        let summary = after.isEmpty ? "Move column \(cur.name) of \(table) first" : "Move column \(cur.name) of \(table) after \(after)"
        Task { await DdlRunner.perform(DdlAction(title: "Move column", summary: summary, sql: sql), in: self, sheet: f) }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
  }

  func openPrimaryKeyEditor() {
    guard let schema else { return }
    let current = schema.columns.filter { $0.keyPosition > 0 }.sorted { $0.keyPosition < $1.keyPosition }.map(\.name)
    let sheet = FormSheetController(
      title: "Primary key of \(table)", subtitle: "The key columns in order; none drops the key.", submitTitle: "Set")
    sheet.addList(id: "columns", label: "Key columns", values: current, options: schema.columns.map(\.name))
    if driver == .sqlite {
      sheet.disable(id: "columns", reason: sqliteWhy)
    }
    sheet.onSubmit = { [weak self] f in
      guard let self, let schema = self.schema else { return }
      let names = f.names("columns")
      if let bad = names.first(where: { schema.column(named: $0) == nil }) {
        f.showError("\(bad) is not a column of \(table)")
        return
      }
      let existing = schema.indexes.first { $0.kind == .primary }.flatMap { $0.constraint ?? $0.name }
      do {
        let sql = try Ddl.setPrimaryKey(driver, table: table, columns: names, existing: existing)
        let summary = names.isEmpty ? "Drop the primary key of \(table)" : "Set the primary key of \(table) to (\(names.joined(separator: ", ")))"
        Task {
          await DdlRunner.perform(
            DdlAction(title: names.isEmpty ? "Drop primary key" : "Set primary key", summary: summary, sql: sql, destructive: names.isEmpty),
            in: self, sheet: f)
        }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
  }

  // MARK: Definition (a view, routine or trigger)

  private func definitionAction(_ a: StructureAction) {
    if a == .edit {
      editDefinitionInQuery()
    } else {
      toast?("A \(object.kind.word.lowercased()) is changed in a query view (Edit); its actions are in the Table menu")
    }
  }

  /// The definition in a query view, to change and run
  func editDefinitionInQuery() {
    guard let definition else {
      loadDefinition()
      toast?("The definition is still loading: try again in a moment")
      return
    }
    onNewQuery?(definition)
  }

  // MARK: Indexes

  private func index(tag: Int) -> IndexInfo? {
    guard let schema, tag >= CatalogTable.Tag.index else { return nil }
    let i = tag - CatalogTable.Tag.index
    let all = schema.indexes
    return i < all.count ? all[i] : nil
  }

  private func indexesAction(_ a: StructureAction, tag: Int) {
    guard schema != nil else {
      toast?("The structure is still loading")
      return
    }
    let ix = index(tag: tag)
    switch a {
    case .add: openIndexEditor()
    case .check: openCheckEditor()
    case .drop: if let ix { dropIndex(ix) } else { toast?("Pick an index first") }
    case .rename, .edit: if let ix { renameIndex(ix) } else { toast?("Pick an index first") }
    case .primaryKey: toast?("The primary key is changed from Structure")
    case .move: break
    }
  }

  func openIndexEditor() {
    guard let schema else { return }
    let sheet = FormSheetController(title: "New index on \(table)", submitTitle: "Create")
    sheet.addText(id: "name", label: "Name")
    sheet.addToggle(id: "unique", label: "Unique", on: false)
    sheet.addList(id: "columns", label: "Columns", options: schema.columns.map(\.name))
    let methods: [String] =
      switch driver {
      case .postgres: ["btree", "hash", "gin", "gist", "brin", "spgist"]
      case .mysql, .mariadb: ["BTREE", "HASH", "FULLTEXT", "SPATIAL"]
      case .sqlite: []
      }
    sheet.addPick(id: "method", label: "Method", options: methods, freeText: true)
    sheet.addText(id: "where", label: "Partial (WHERE)", hint: "a condition: status = 'open'")
    if driver == .sqlite {
      sheet.disable(id: "method", reason: "SQLite indexes have no method")
    }
    if isMySQL {
      sheet.disable(id: "where", reason: "MySQL has no partial indexes")
    }
    sheet.onSubmit = { [weak self] f in
      guard let self else { return }
      let name = f.text("name")
      let cols = f.names("columns")
      guard !name.isEmpty else {
        f.showError("A name is needed")
        return
      }
      guard !cols.isEmpty else {
        f.showError("At least one column is needed")
        return
      }
      do {
        let unique = f.on("unique")
        let sql = try Ddl.createIndex(
          driver, table: table, name: name, unique: unique, columns: cols, method: f.value("method"), where: f.value("where"))
        let summary =
          "Create \(unique ? "unique " : "")index \(name) on \(rowsText) (\(cols.joined(separator: ", "))).\nLocks the table while it builds; Esc cancels."
        Task { await DdlRunner.perform(DdlAction(title: "Create index", summary: summary, sql: sql), in: self, sheet: f) }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
  }

  func openCheckEditor() {
    if driver == .sqlite {
      toast?("SQLite takes check constraints only at CREATE TABLE; create a new table and copy the rows")
      return
    }
    let sheet = FormSheetController(title: "New check constraint on \(table)", submitTitle: "Add")
    sheet.addText(id: "name", label: "Name", hint: "optional")
    sheet.addText(id: "condition", label: "Condition", hint: "total >= 0")
    sheet.onSubmit = { [weak self] f in
      guard let self else { return }
      let cond = f.text("condition")
      guard !cond.isEmpty else {
        f.showError("A condition is needed")
        return
      }
      do {
        let sql = try Ddl.addCheck(driver, table: table, name: f.value("name"), condition: cond)
        let summary = "Add the check constraint \(f.text("name"))(\(cond)) to \(table).\nEvery row must satisfy it already."
        Task { await DdlRunner.perform(DdlAction(title: "Add check constraint", summary: summary, sql: sql), in: self, sheet: f) }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
  }

  private func kindWord(_ ix: IndexInfo) -> String {
    switch ix.kind {
    case .check: "check constraint"
    case .exclusion: "exclusion constraint"
    case .unique: "unique index"
    default: "index"
    }
  }

  private func dropIndex(_ ix: IndexInfo) {
    if ix.kind == .primary {
      toast?("The primary key is changed from Structure (Primary Key…)")
      return
    }
    do {
      let sql =
        ix.kind == .check || ix.kind == .exclusion
        ? try Ddl.dropConstraint(driver, table: table, name: ix.name ?? "")
        : try Ddl.dropIndex(driver, table: table, name: ix.name ?? "", constraint: ix.constraint)
      let summary = "Drop the \(kindWord(ix)) \(ix.name ?? "") of \(table)."
      Task { await DdlRunner.perform(DdlAction(title: "Drop index", summary: summary, sql: sql, destructive: true), in: self) }
    } catch {
      DdlRunner.refused(error, in: self)
    }
  }

  private func renameIndex(_ ix: IndexInfo) {
    if ix.kind == .primary {
      toast?("The primary key is changed from Structure (Primary Key…)")
      return
    }
    Task { [weak self] in
      guard let self else { return }
      guard let name = await promptName("Rename \(ix.name ?? "index")", "New name", initial: ix.name), name != ix.name else { return }
      do {
        let sql =
          ix.kind == .check || ix.kind == .exclusion
          ? try Ddl.renameConstraint(driver, table: table, name: ix.name ?? "", to: name)
          : try Ddl.renameIndex(driver, table: table, index: ix, to: name)
        let summary =
          "Rename the \(kindWord(ix)) \(ix.name ?? "") of \(table) to \(name)\(sql.contains(";") ? " (dropped and created again)" : "")"
        await DdlRunner.perform(DdlAction(title: "Rename index", summary: summary, sql: sql), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  // MARK: Foreign keys

  private func foreignKey(tag: Int) -> ForeignKey? {
    guard let schema, tag >= CatalogTable.Tag.foreignKey, tag < CatalogTable.Tag.dependent else { return nil }
    let i = tag - CatalogTable.Tag.foreignKey
    return i < schema.foreignKeys.count ? schema.foreignKeys[i] : nil
  }

  private func foreignKeysAction(_ a: StructureAction, tag: Int) {
    guard schema != nil else {
      toast?("The structure is still loading")
      return
    }
    let fk = foreignKey(tag: tag)
    switch a {
    case .add: openForeignKeyEditor(editing: nil)
    case .edit: if let fk { openForeignKeyEditor(editing: fk) } else { toast?("Pick a foreign key first") }
    case .drop:
      if driver == .sqlite { toast?(sqliteFKWhy) } else if let fk { dropForeignKey(fk) } else { toast?("Pick a foreign key first") }
    case .rename: if let fk { renameForeignKey(fk) } else { toast?("Pick a foreign key first") }
    case .primaryKey, .move, .check: break
    }
  }

  /// MySQL needs an index starting with the key's columns
  private func indexed(_ cols: [String]) -> Bool {
    guard let schema else { return true }
    return schema.indexes.contains { ix in
      ix.kind != .check && ix.kind != .exclusion && ix.columns.count >= cols.count && Array(ix.columns.prefix(cols.count)) == cols
    }
  }

  /// The ADD statement, with MySQL's index before it when the columns have none (noted)
  private func addForeignKeySQL(_ key: DdlForeignKey) throws -> (sql: String, note: String?) {
    let add = try Ddl.addForeignKey(driver, table: table, key: key)
    guard isMySQL, !indexed(key.columns), let first = key.columns.first else { return (add, nil) }
    let ixName = "fk_\(table)_\(first)"
    let ix = try Ddl.createIndex(driver, table: table, name: ixName, unique: false, columns: key.columns, method: nil, where: nil)
    return ("\(ix);\n\(add)", "\nAnd an index \(ixName) on (\(key.columns.joined(separator: ", "))), which MySQL requires.")
  }

  func openForeignKeyEditor(editing cur: ForeignKey?) {
    guard let schema else { return }
    if driver == .sqlite {
      toast?(sqliteFKWhy)
      return
    }
    let sheet = FormSheetController(
      title: cur.map { "Foreign key \($0.name ?? "")" } ?? "New foreign key on \(table)", submitTitle: cur == nil ? "Add" : "Change")
    sheet.addText(id: "name", label: "Name", value: cur?.name, hint: "optional")
    sheet.addList(id: "columns", label: "Columns", values: cur?.columns ?? [], options: schema.columns.map(\.name))
    sheet.addPick(id: "ref_table", label: "References", value: cur?.refTable, options: tablesProvider?() ?? [], freeText: true)
    sheet.addList(id: "ref_columns", label: "Referenced columns", values: cur?.refColumns ?? [])
    sheet.addPick(id: "on_delete", label: "ON DELETE", value: cur?.onDelete, options: fkActions, freeText: true)
    sheet.addPick(id: "on_update", label: "ON UPDATE", value: cur?.onUpdate, options: fkActions, freeText: true)
    sheet.addToggle(id: "deferrable", label: "Deferrable", on: false)
    sheet.addToggle(id: "deferred", label: "Initially deferred", on: false)
    if driver != .postgres {
      sheet.disable(id: "deferrable", reason: "Only PostgreSQL has deferrable constraints")
      sheet.disable(id: "deferred", reason: "Only PostgreSQL has deferrable constraints")
    }
    let old = cur?.name
    sheet.onSubmit = { [weak self] f in
      guard let self else { return }
      let cols = f.names("columns")
      let rcols = f.names("ref_columns")
      let ref = f.text("ref_table")
      guard !cols.isEmpty else {
        f.showError("At least one column is needed")
        return
      }
      guard !ref.isEmpty else {
        f.showError("The referenced table is needed")
        return
      }
      guard rcols.count == cols.count else {
        f.showError("As many referenced columns as columns are needed")
        return
      }
      let key = DdlForeignKey(
        name: f.value("name"), columns: cols, refTable: ref, refColumns: rcols, onDelete: f.value("on_delete"),
        onUpdate: f.value("on_update"), deferrable: f.on("deferrable"), initiallyDeferred: f.on("deferred"))
      do {
        var (sql, note) = try addForeignKeySQL(key)
        if let old {
          sql = try Ddl.dropForeignKey(driver, table: table, name: old) + ";\n" + sql
        }
        let summary =
          old.map { "Change the foreign key \($0) of \(table): (\(cols.joined(separator: ", "))) references \(ref) (\(rcols.joined(separator: ", "))), by dropping it and adding it again.\(note ?? "")" }
          ?? "Add a foreign key to \(table): (\(cols.joined(separator: ", "))) references \(ref) (\(rcols.joined(separator: ", "))).\nEvery row must satisfy it already.\(note ?? "")"
        Task {
          await DdlRunner.perform(
            DdlAction(title: old == nil ? "Add foreign key" : "Change foreign key", summary: summary, sql: sql, destructive: old != nil),
            in: self, sheet: f)
        }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
  }

  private func dropForeignKey(_ fk: ForeignKey) {
    guard let name = fk.name else {
      toast?("The key has no name to drop it by")
      return
    }
    do {
      let sql = try Ddl.dropForeignKey(driver, table: table, name: name)
      let summary = "Drop the foreign key \(name) of \(table) (references \(fk.refTable ?? ""))."
      Task { await DdlRunner.perform(DdlAction(title: "Drop foreign key", summary: summary, sql: sql, destructive: true), in: self) }
    } catch {
      DdlRunner.refused(error, in: self)
    }
  }

  private func renameForeignKey(_ fk: ForeignKey) {
    if driver == .sqlite {
      toast?(sqliteFKWhy)
      return
    }
    guard let oldName = fk.name else {
      toast?("The key has no name")
      return
    }
    Task { [weak self] in
      guard let self else { return }
      guard let name = await promptName("Rename \(oldName)", "New name", initial: oldName), name != oldName else { return }
      do {
        let sql: String
        if driver == .postgres {
          sql = try Ddl.renameConstraint(driver, table: table, name: oldName, to: name)
        } else {
          let key = DdlForeignKey(
            name: name, columns: fk.columns, refTable: fk.refTable ?? "", refColumns: fk.refColumns, onDelete: fk.onDelete,
            onUpdate: fk.onUpdate)
          sql = try Ddl.dropForeignKey(driver, table: table, name: oldName) + ";\n" + (try addForeignKeySQL(key)).sql
        }
        let summary = "Rename the foreign key \(oldName) of \(table) to \(name)\(driver == .postgres ? "" : " (dropped and added again)")"
        await DdlRunner.perform(DdlAction(title: "Rename foreign key", summary: summary, sql: sql), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  // MARK: Information

  /// The action rows under the facts (this frontend's tags)
  func infoActionRows() -> [CatalogRow] {
    var rows: [CatalogRow] = [CatalogRow(cells: [""], span: true, heading: false, tag: CatalogTable.Tag.none)]
    rows.append(CatalogRow(cells: ["Actions"], span: true, heading: true))
    func row(_ text: String, _ tag: Int) { rows.append(CatalogRow(cells: [text], span: true, tag: tag)) }
    guard object.kind == .table else {
      row("Edit in a query view", InfoTag.editDefinition)
      return rows
    }
    if let a = info?.autoIncrement {
      row(a.hasCounter ? "Set the next auto-increment value…" : "Set the next auto-increment value (no counter: the maximum + 1)", InfoTag.autoIncrement)
    }
    if driver == .postgres || isMySQL { row("Set the comment…", InfoTag.comment) }
    if isMySQL {
      row("Set the engine…", InfoTag.engine)
      row("Set the charset and collation…", InfoTag.charset)
    }
    row("Analyze (update the statistics)", InfoTag.analyze)
    if driver == .postgres { row("Vacuum (VACUUM ANALYZE the table)", InfoTag.vacuum) }
    if driver == .sqlite { row("Vacuum (the whole file: SQLite has no per-table VACUUM)", InfoTag.vacuum) }
    if isMySQL { row("Optimize (OPTIMIZE TABLE)", InfoTag.optimize) }
    return rows
  }

  private func infoAction(_ a: StructureAction, tag: Int) {
    if a == .edit {
      _ = runInfoTag(tag)
    } else {
      toast?("Return runs the action under the cursor; the table's actions are in the Table menu")
    }
  }

  /// One of the action rows; false for another tag
  func runInfoTag(_ tag: Int) -> Bool {
    switch tag {
    case InfoTag.autoIncrement: setAutoincrement()
    case InfoTag.comment: setComment()
    case InfoTag.engine: setEngine()
    case InfoTag.charset: setCharset()
    case InfoTag.analyze: maintenance(.analyze)
    case InfoTag.vacuum: maintenance(.vacuum)
    case InfoTag.optimize: maintenance(.optimize)
    case InfoTag.editDefinition: editDefinitionInQuery()
    default: return false
    }
    return true
  }

  func setAutoincrement() {
    guard let counter = info?.autoIncrement else { return }
    guard counter.hasCounter else {
      toast?("A plain INTEGER PRIMARY KEY has no counter: the next rowid is the maximum + 1")
      return
    }
    Task { [weak self] in
      guard let self else { return }
      guard let text = await promptName("Next auto-increment value", "Next value", initial: counter.next >= 0 ? String(counter.next) : "1")
      else { return }
      guard let n = Int64(text) else {
        toast?("Not a number: \(text)")
        return
      }
      do {
        let sql = try Ddl.setAutoincrement(driver, table: table, counter: counter, value: n)
        let note = counter.max >= 0 && n <= counter.max ? " (MySQL refuses a value below the maximum; the rest allow it)" : ""
        await DdlRunner.perform(
          DdlAction(title: "Set auto-increment", summary: "The next \(counter.column) of \(table) becomes \(n)\(note)", sql: sql), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  func setComment() {
    Task { [weak self] in
      guard let self else { return }
      var answer = ScriptDriver.takePromptAnswer()
      if answer == nil {
        answer = await Alerts.prompt("Comment on \(table)", "Comment", initial: info?.comment ?? "", window: view.window)
      }
      guard let text = answer else { return }
      do {
        let sql = try Ddl.setTableOption(driver, table: table, option: .comment, value: text)
        let summary = text.isEmpty ? "Clear the comment of \(table)" : "Set the comment of \(table) to \"\(text)\""
        await DdlRunner.perform(DdlAction(title: "Set comment", summary: summary, sql: sql), in: self)
      } catch {
        DdlRunner.refused(error, in: self)
      }
    }
  }

  func setEngine() {
    let sheet = FormSheetController(title: "Engine of \(table)", submitTitle: "Change")
    sheet.addPick(id: "engine", label: "Engine", value: info?.engine, freeText: true)
    sheet.onSubmit = { [weak self] f in
      guard let self else { return }
      guard let engine = f.value("engine") else {
        f.showError("Pick an engine")
        return
      }
      do {
        let sql = try Ddl.setTableOption(driver, table: table, option: .engine, value: engine)
        let summary = "Change the engine of \(table) to \(engine).\nThe table is rebuilt: it is locked meanwhile."
        Task { await DdlRunner.perform(DdlAction(title: "Set engine", summary: summary, sql: sql), in: self, sheet: f) }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
    let connection = self.connection
    Task { [weak sheet] in
      if let engines = try? await connection.listEngines() { sheet?.setOptions(id: "engine", engines) }
    }
  }

  func setCharset() {
    let sheet = FormSheetController(title: "Charset of \(table)", submitTitle: "Change")
    sheet.addText(id: "charset", label: "Charset", value: info?.charset, hint: "utf8mb4")
    sheet.addPick(id: "collation", label: "Collation", value: info?.collation, freeText: true)
    sheet.onSubmit = { [weak self] f in
      guard let self else { return }
      guard let cs = f.value("charset") else {
        f.showError("A charset is needed")
        return
      }
      do {
        let coll = f.value("collation")
        let sql = try Ddl.setTableOption(driver, table: table, option: .charset, value: cs, value2: coll)
        let summary = "Set the default charset of \(table) to \(cs)\(coll.map { " with collation \($0)" } ?? "") (existing columns keep theirs)"
        Task { await DdlRunner.perform(DdlAction(title: "Set charset", summary: summary, sql: sql), in: self, sheet: f) }
      } catch {
        DdlRunner.refused(error, in: self, sheet: f)
      }
    }
    presentAsSheet(sheet)
    let connection = self.connection
    let charset = info?.charset
    Task { [weak sheet] in
      if let colls = try? await connection.listCollations(charset: charset) { sheet?.setOptions(id: "collation", colls) }
    }
  }

  func maintenance(_ kind: DdlMaintenance) {
    do {
      let sql = try Ddl.maintenance(driver, table: table, kind: kind)
      let title = kind == .analyze ? "Analyze" : kind == .vacuum ? "Vacuum" : "Optimize"
      let summary =
        kind == .vacuum && driver == .sqlite
        ? "Rebuild the whole database file (SQLite's VACUUM is not per table).\nLocks the database while it runs; Esc cancels."
        : "\(title) \(rowsText).\nLocks the table while it runs; Esc cancels."
      Task { await DdlRunner.perform(DdlAction(title: title, summary: summary, sql: sql), in: self) }
    } catch {
      DdlRunner.refused(error, in: self)
    }
  }
}

/// A StructureAction as a menu item's representedObject
final class StructureActionBox: NSObject {
  let action: StructureAction
  init(_ action: StructureAction) { self.action = action }
}
