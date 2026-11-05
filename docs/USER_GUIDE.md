# TextScope user guide

TextScope is a desktop text/log viewer with ordered filters and eight marker
types. Each document has its own window. It is not a text editor, a live-tail
viewer, or a certified exact reproduction of TextAnalysisTool.NET.

For installation packages and Homebrew/pacman commands, see the
[release guide](RELEASING.md). For building from source, see the
[developer guide](DEVELOPER_GUIDE.md).

## Opening and keeping a document

Use File > Open, drop a file on the window, or paste text from the clipboard.
File > New window opens another document window. Recent file/filter lists and
reload are available from the File menu.

Normal opens capture an immutable snapshot: later edits, replacement or deletion
of the source do not change the open document. Large inputs, indexes and result
sets can spill to temporary disk storage. Keep sufficient disk space and a
writable working directory; the application reports capture/read/write errors.
Owned temporary files are removed when their owners are destroyed normally.

`--direct` explicitly reads the original file instead of capturing it. Detected
source changes invalidate reads; reopen the file to continue. This is not a frozen
snapshot or automatic follow/tail mode, and is never selected silently because
a snapshot would be large. Stateful encodings require snapshot mode.
On Windows, the open direct-read handle prevents deletion or replacement of the
source until the document is closed; writes are still allowed. Normal immutable
captures do not retain this handle.
Change detection checks file identity, size and timestamps, not every byte.
Same-size rewrites with unchanged timestamps can escape these checks. Use the
default immutable snapshot when a stable document is required.

While loading, an explicitly labelled prefix preview is not the final document.
Selection, navigation and analysis become available for the completed snapshot.

## Filters and colors

Add a filter with Filters > Add New Filter or by double-clicking a source line.
Choose literal text, a native PCRE2 regex, or a marker predicate. Configure
case sensitivity, exclusion, foreground/background colors and an optional
description.

- Enabled including filters combine as OR.
- Enabled excluding filters remove matching lines afterward.
- With no enabled including filters, all lines start eligible.
- The topmost matching colored including filter takes color priority.
- Selection colors override filter colors.

Show Only Filtered Lines hides nonaccepted lines. Otherwise they remain visible
as dimmed context. Switching modes retains selection by original source ID.
Showing all lines keeps the latest selected line and scrolls it into view; it
does not restore an older selection from before filtering.
Filter counts, letter IDs, predicates and colors appear in the compact list.
Changing enabled state, colors or order reuses cached memberships when available.

In the filter list, Space toggles selected filters, Enter edits, and Delete
removes. Drag to reorder. Double-click edits a filter; color menus provide
default/named/custom values, and canceling a custom-color chooser preserves the
previous value. The editor shows Description below Text, followed by the
exclusion/case/regex options. Named colors remain alphabetical; text-color
menus use theme-colored labels with small swatches on a consistent background;
background menus use full-row swatches, and Custom is last. Equivalent Qt aliases
(such as Gray/Grey or Aqua/Cyan) share one alphabetical entry without changing
stored color values.
The checkbox indicates whether a filter is enabled; selecting its row alone
does not enable it.

Logs and filter rows share the chosen font and zoom. New Windows profiles use
10-point Consolas (or the system fixed-width font) with medium weight; saved
font choices are preserved. Nonmatching context is dimmed without washing out
the text. Point sizing follows display scaling; use Preferences and zoom to
adjust readability rather than changing screen-resolution assumptions.

Multi-filter editing propagates fields changed relative to the first selected
filter. It is not a mixed-value editor certified against the original app.

## Markers, selection and navigation

A source line can carry more than one of eight marker types. Toggle markers on
selected lines, navigate by marker, or use marker filters. The gutter can be
always visible, hidden, or visible only when markers are in use.

Displayed line numbers refer to the original source, not a renumbered filtered
list. Shift extends a selection; Command on macOS or Control elsewhere adds
individual lines. Go to line and keyboard navigation address original lines.
Dragging the scrollbar is proportional for very large documents.

Find searches the full document, including lines hidden by filters. Filter-letter
and marker shortcuts apply in the line pane, not while typing in an editor.

## Keyboard shortcuts

| Action | macOS | Windows/Linux |
|---|---|---|
| Open / Find / Copy / Paste | Cmd+O / F / C / V | Ctrl+O / F / C / V |
| Add filter / New window | Cmd+N / Cmd+Shift+N | Ctrl+N / Ctrl+Shift+N |
| Filtered-only | Cmd+Shift+H | Ctrl+H |
| Toggle marker 1-8 | Cmd+1-8 | Ctrl+1-8 |
| Next/previous accepted line | Space / Shift+Space | Same |
| Next/previous marker 1-8 | 1-8 / Shift+1-8 | Same |
| Next/previous filter A-Z | A-Z / Shift+A-Z | Same |
| Toggle filter A-Z | Cmd+Shift+A-Z | Ctrl+Shift+A-Z |
| Selected-filter navigation | F8 / Shift+F8 | Same |
| Go to line | Cmd+L | Ctrl+L |

Cmd+H remains macOS Hide. Cmd+Shift+H is reserved for filtered-only mode;
toggle filter H with its checkbox instead. In the filter pane, Cmd/Ctrl+arrow
keys move the dock to the corresponding side. Conventional commands use Qt's
platform shortcuts.

## Copying and saving

Copy selected lines as complete text, optionally with line numbers. Rich HTML
clipboard output includes configured filter colors. Save Current Lines writes
the current visible set as complete decoded UTF-8 lines with LF terminators.
It does not preserve the source encoding or original newline style.

Very long lines are visibly truncated for rendering (16,384 UTF-16 code units
by default); filtering, find, copy and export still use complete lines.
Clipboard payloads occupy memory; use file export for large results.

## Filter files and preferences

Load, append, save and copy/paste `.tat` filter sets. Opening/dropping a `.tat`
through the GUI treats it as filters; a positional CLI filename is always an
input text document.

TextScope reads/writes TextAnalysisTool.NET text/marker XML attributes, colors,
descriptions and filtered-only state. Unknown attributes survive round trips.
Unsupported filter types and invalid regexes are explicit errors. Marker XML
uses numbers 1-8; exact original-binary marker numbering and cross-application
clipboard behavior have not been certified.

Preferences include font, zoom, tab width, encoding, colors, marker/line-number
display, copy line numbers, replacement confirmation, title display, remembered
layout and dirty-filter prompting. Native QSettings stores application state;
preferences XML exports use the `TextScope` root. Older `TextAnalysisTool.Qt`
exports remain readable. Original .NET registry/preferences schemas are not
imported.

When TextScope has no saved settings, it copies legacy TextAnalysisToolQt
settings on startup. It does not overwrite existing TextScope settings.

Adding, deleting or editing filters marks a file-backed set as unsaved.
Whether toggling/reordering marks it unsaved is configurable. Ad-hoc sets
without a backing filter file do not trigger an exit save prompt.

## Command line

Use the `tscope` launcher (Homebrew/pacman put it on PATH):

```sh
tscope /path/to/log.txt --filters errors.tat --line 120
tscope --clipboard
tscope /path/to/log.txt --encoding GB18030
```

For local development use `./build/tscope` (`./build/tscope.exe` on Windows).
Standalone Linux/Windows archives include it in `bin`; macOS bundles include
`textscope.app/Contents/MacOS/tscope`. See the [release guide](RELEASING.md#standalone-packages)
for optional PATH setup.

The launcher returns after starting a new TextScope process, forwarding arguments
and preserving the current working directory for relative file/filter/config
paths. `-h`/`--help` and `-v`/`--version` print without opening a window.
Missing applications or launch failures return exit code 1 with a terminal error.
Exit code 0 means the process started, not that its file load succeeded; subsequent
application errors are reported by the GUI. There is no `--wait` or window reuse.
The child is detached with terminal streams disconnected.

The original executable still works: on macOS use
`textscope.app/Contents/MacOS/textscope`; on Linux/Windows use
`textscope`/`textscope.exe`. It runs directly rather than through the detached launcher.

| Option | Meaning |
|---|---|
| `--filters file.tat` | Load filters; repeated options append in order |
| `--config preferences.xml` | Import application preferences XML |
| `--line N` | Navigate to a positive, original 1-based line |
| `--clipboard` | Use clipboard text as the source |
| `--encoding NAME` | Select a supported text codec; a BOM takes precedence |
| `--direct` | Read the mutable original file explicitly |
| `--` | Treat remaining arguments as paths rather than options |

Legacy `/Filters:`, `/Config:`, `/Line:` and `/Clipboard` aliases are accepted.
When both clipboard and file input are supplied, the first source wins.
Only one file source is allowed per window.

## Limitations and troubleshooting

- **Regex:** PCRE2 is not .NET Regex. Syntax, Unicode/case handling and captures
  can differ; balancing groups and class subtraction are not translated.
  Successful compilation is not proof of identical results. Review imported
  patterns; there is no automatic .NET fallback.
- **Plug-ins:** native Qt parser plug-ins are trusted executable code. Original
  `TATPlugin_*.dll` managed assemblies are not loaded.
- **Accessibility:** controls have accessible names, but the custom-painted
  viewport does not expose a complete screen-reader text/row interface.
  Copy/export provides full text; this does not replace full accessibility.
- **macOS Open:** the application uses Qt's window-modal chooser instead of
  the shared native file panel.
- **Direct mode errors:** reopen a changed source; do not treat an obsolete index
  as current data.
- **Disk errors:** ensure the working directory is writable and has room for
  snapshots/indexes/results. Do not assume a small resident cache means small
  temporary-disk use.
- **Packages:** a managed Homebrew/pacman install requires its dependencies;
  it is not portable like a standalone bundled app. macOS downloads are not
  notarized; approve only trusted builds using macOS's per-app security controls.

Potential features are in the [roadmap](ROADMAP.md); release verification gaps
are in the [release guide](RELEASING.md#outstanding-release-readiness).
