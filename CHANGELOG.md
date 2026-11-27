# Changelog

## Unreleased
- NETCONF request builder in the docked panel: for the selected node, build a
  `get` / `get-config` (subtree filter), `edit-config` (merge, replace, create, delete,
  remove on running or candidate), `rpc` or `action` request. Fields for list keys and
  values come from the model (enum, boolean and identityref lists; ranges, lengths,
  patterns and mandatory leaves checked); augments into other modules get the right
  namespaces and keys. *Open in new tab* puts the request in a new XML document; *Copy*
  copies it.
- Dark mode: native scrollbars and form controls in the viewer are dark too.

## 1.1.0 - 2026-11-27
- Docked viewer: Show YANG tree (Ctrl+Alt+Y) now opens the tree in a Notepad++ panel
  (Microsoft Edge WebView2) with the same page, features and style as the browser view.
  The panel can be docked on any side or floated, and is restored at startup if it was
  open when Notepad++ closed.
- New menu item *Show YANG tree in browser* for the separate-window view.
- Settings: *Use docked panel* (off = Show YANG tree opens the browser, as in 1.0) and
  *Panel follows active tab*, as menu ticks and `panel=` / `follow=` in `YangTreeView.ini`
  (*Search paths...* is now *Search paths and settings...*).
- Docked panel follows Notepad++'s dark mode (live), stacks the details pane under the tree
  when narrow, and has a pop-out button (↗). Esc returns to the editor; the Show YANG tree
  shortcut works inside the panel too.
- Docked panel: click *file : line* in the details pane to open the file at that line;
  refreshes on save and follows the active `.yang` tab, keeping expanded nodes, selection
  and scroll position per module; syntax errors during these refreshes show as a note in
  the panel instead of a message box.
- CLI pages don't include absolute file paths (the plugin's do, for jump to source).
- Viewer: narrow windows get the stacked layout too; legend icons in the details pane are
  coloured again.
- Without the WebView2 Runtime the tree opens in the browser as before.

## 1.0.0 - 2026-11-20
- First release.
- Show YANG tree (Ctrl+Alt+Y): interactive collapsible HTML tree of the active module,
  groupings resolved across submodules and imports, search, schema paths, details pane,
  light and dark themes.
- Install YANG syntax highlighting (UDL, light or dark colours to match Notepad++).
- Configurable search paths (`YangTreeView.ini`) and `YANG_MODPATH`.
- Standalone `yang2html.exe` CLI with the same engine.
