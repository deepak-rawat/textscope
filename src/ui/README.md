# Qt Widgets interface

See the [developer guide](../../docs/DEVELOPER_GUIDE.md) for build and architecture
context, and the [user guide](../../docs/USER_GUIDE.md) for visible behavior.
The UI library uses Qt Widgets, Qt Gui and Qt Core. Rendering work is dispatched
to `QThreadPool`; target dependencies and AUTOMOC sources are maintained in the
project's CMake configuration.

`MainWindow` provides a central `LogView`, an ordered, multi-select, draggable
filter table and a bottom filter dock. Actions are discoverable by `objectName`,
which is their exact `commandRequested` string. Commands are passive requests
for the session/controller; dock position actions additionally move the UI dock.
Recent items emit `open-recent:PATH` or `filters-recent:PATH`. The close signal's
`bool*` is valid only during synchronous, direct signal delivery; set it to false
to veto closing. Queued connections cannot implement close confirmation.

`LogView` keeps selections and anchors as zero-based source IDs even while
filtering. Displayed numbers are one-based. `selectAll()` shares a compact
`LineSet`, rather than enumerating rows. `firstVisibleRank()` reports the exact
64-bit origin within the visible sequence. Thumb dragging maps proportionally
to a bounded integer scrollbar; arrows, wheel and page actions move exact rows.
Marker visibility is 0 = always, 1 = never, 2 = when any marker is used.
`zoomChanged(int)` updates the main-window zoom status, including wheel zoom.
Controllers may connect it to synchronize persisted zoom and filter-pane fonts.
`setZoom()` emits only when the clamped value changes, so writing the notified
value back does not recurse. Zero-delta wheel end events do not change zoom.

Plain A–Z navigate filters, 1–8 navigate markers, and Space navigates the active
filter (`filter = -1`); Shift reverses navigation. Command/Control+1–8 toggle
markers and Command/Control+Shift+A–Z toggle filters. These keys are scoped to
the log view, not text editors or the filter table. Qt standard key sequences
are used for platform conventions. On macOS Qt's ControlModifier represents
Command; filtered-only is Command+Shift+H, never the OS hide shortcut Command+H.
In `MainWindow`, the filtered-only action reserves Command/Control+Shift+H;
toggle filter H through its table checkbox instead.
Both Shift+digit key codes and shifted US-layout symbols `!@#$%^&*` navigate
the corresponding previous marker.

Snapshot reads and bounded `QStaticText` layout preparation run off the GUI thread. Cache
results are revision-guarded, pending jobs are bounded and obsolete work is
discarded. At most `setMaxCharacters()` UTF-16 characters are shown per row
(default 16,384; configurable from 1 to 1,048,576), with a visible truncation
notice. Tab expansion is also bounded. Full-source operations are unaffected;
double-click reads the complete line asynchronously before requesting a filter.
The first enabled, including, colored matching filter wins. Selection colors
override filter colors and nonaccepted lines are dimmed. Rows never wrap.

`setLoadingPreview(QString)` or `setLoadingPreview(const QStringList&)` is a separate, explicitly labelled **not-final**
prefix preview. The controller should supply decoded text from at most the first
64 KiB of source bytes; the UI also caps it to 65,536 UTF-16 characters. It never
exposes a mutable/partial `Snapshot`, performs no GUI-thread file reads, prepares
text layout in workers, and disables source selection/analysis/navigation.
The next `setDocument()` discards the preview and publishes the final snapshot;
`clearLoadingPreview()` explicitly dismisses it when an open is cancelled or fails.
The loading label is painted immediately; actual capture/index time remains the
controller/core's responsibility rather than a UI timing guarantee.

Accessible names/descriptions are provided for the window, filter table,
viewport, status and editor controls. The filter table uses Qt's native
accessibility. **Limitation:** the painted viewport does not expose a complete
virtual text/row `QAccessibleInterface`; screen readers cannot enumerate its
source text. Copy/export remain the full-text access path. This is not full
screen-reader text-view parity.

`tests/ui/widgets_test.cpp` is a standalone QtTest executable (`QTEST_MAIN`);
run with `QT_QPA_PLATFORM=offscreen` in headless environments.
