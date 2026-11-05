#include "ui/ui.hpp"
#include "filterview.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QUrl>

namespace textscope {
namespace {
class StatusCancelButton final : public QPushButton {
public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override {
        auto size = QPushButton::sizeHint();
        size.setHeight(fontMetrics().height() + 6);
        return size;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
};
}
struct MainWindow::Impl {
    LogView* log = nullptr;
    FilterModel* model = nullptr;
    QTableView* table = nullptr;
    QDockWidget* dock = nullptr;
    QLabel* status = nullptr;
    QLabel* activity = nullptr;
    QPushButton* cancel = nullptr;
    QAction* cancelAction = nullptr;
    QMenu* recentFiles = nullptr;
    QMenu* recentFilters = nullptr;
    QString statusSource;
    LineId statusSelected = 0;
    LineId statusFiltered = 0;
    LineId statusTotal = 0;
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), impl_(std::make_unique<Impl>()) {
    setWindowTitle(tr("TextAnalysisTool"));
    setObjectName("textAnalysisTool");
    setAccessibleName(tr("TextScope"));
    setAcceptDrops(true);
    resize(1100, 760);
    impl_->log = new LogView(this);
    setCentralWidget(impl_->log);
    for (int marker = 0; marker < 8; ++marker) {
        auto* action = new QAction(tr("Toggle marker %1").arg(marker + 1), this);
        action->setObjectName(QString("toggle-marker-%1").arg(marker + 1));
        action->setShortcut(QKeySequence(QString("Ctrl+%1").arg(marker + 1)));
        addAction(action);
        connect(action, &QAction::triggered, impl_->log, [this, marker] { emit impl_->log->toggleMarkerRequested(marker); });
    }
    impl_->model = new FilterModel(this);
    auto* filterView = new FilterView(this);
    impl_->table = filterView;
    filterView->command = [this](const QString& command) { emit commandRequested(command); };
    impl_->table->setObjectName("filterView");
    impl_->table->setAccessibleName(tr("Ordered filters"));
    impl_->table->setModel(impl_->model);
    const auto syncFont = [this](const QFont& font) {
        impl_->table->setFont(font);
        impl_->table->verticalHeader()->setDefaultSectionSize(QFontMetrics(font).height() + 4);
    };
    connect(impl_->log, &LogView::textFontChanged, this, syncFont);
    syncFont(impl_->log->font());
    impl_->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    impl_->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    impl_->table->setDragDropMode(QAbstractItemView::InternalMove);
    impl_->table->setDefaultDropAction(Qt::MoveAction);
    impl_->table->setDragEnabled(true);
    impl_->table->setAcceptDrops(true);
    impl_->table->setDropIndicatorShown(true);
    impl_->table->setDragDropOverwriteMode(false);
    impl_->table->setSortingEnabled(false);
    impl_->dock = new QDockWidget(tr("Filters"), this);
    impl_->dock->setObjectName("filtersDock");
    impl_->dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    impl_->dock->setWidget(impl_->table);
    impl_->dock->setTitleBarWidget(new QWidget(impl_->dock));
    addDockWidget(Qt::BottomDockWidgetArea, impl_->dock);
    resizeDocks({impl_->dock}, {140}, Qt::Vertical);

    auto add = [&](QMenu* menu, const QString& command, const QString& text,
        const QKeySequence& shortcut = {}, bool checkable = false, bool checked = false) {
        auto* action = menu->addAction(text);
        action->setObjectName(command);
        action->setMenuRole(QAction::NoRole);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WindowShortcut);
        action->setCheckable(checkable);
        action->setChecked(checked);
        connect(action, &QAction::triggered, this, [this, command] {
            if (command.startsWith("dock-")) {
                const auto area = command == "dock-left" ? Qt::LeftDockWidgetArea :
                    command == "dock-right" ? Qt::RightDockWidgetArea :
                    command == "dock-top" ? Qt::TopDockWidgetArea : Qt::BottomDockWidgetArea;
                addDockWidget(area, impl_->dock);
                impl_->dock->show();
            }
            emit commandRequested(command);
        });
        return action;
    };
    auto* file = menuBar()->addMenu(tr("&File"));
    add(file, "new-window", tr("New &window"), QKeySequence("Ctrl+Shift+N"));
    add(file, "open", tr("&Open…"), QKeySequence::Open);
    impl_->recentFiles = file->addMenu(tr("Recent files"));
    add(file, "reload", tr("&Reload"), QKeySequence("Ctrl+R"));
    add(file, "save-lines", tr("&Save Current Lines…"), QKeySequence::Save);
    file->addSeparator();
    add(file, "load-filters", tr("Load filters…"));
    add(file, "save-filters", tr("Save filters"), QKeySequence("Ctrl+Shift+S"));
    add(file, "save-filters-as", tr("Save filters as…"));
    add(file, "append-filters", tr("Append filters…"));
    impl_->recentFilters = file->addMenu(tr("Recent filters"));
    file->addSeparator();
    auto* quit = add(file, "quit", tr("&Quit"), QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    auto* edit = menuBar()->addMenu(tr("&Edit"));
    add(edit, "copy", tr("&Copy selected lines"), QKeySequence::Copy);
    add(edit, "paste", tr("&Paste text"), QKeySequence::Paste);
    add(edit, "copy-filters", tr("Copy filters"));
    add(edit, "paste-filters", tr("Paste filters"));
    add(edit, "select-all", tr("Select &all lines"), QKeySequence::SelectAll);
    edit->addSeparator();
    add(edit, "find", tr("&Find…"), QKeySequence::Find);
    add(edit, "find-next", tr("Find next"), QKeySequence::FindNext);
    add(edit, "find-previous", tr("Find previous"), QKeySequence::FindPrevious);
    add(edit, "goto", tr("&Go to line…"), QKeySequence("Ctrl+L"));
    auto* preferences = add(edit, "preferences", tr("&Preferences…"), QKeySequence::Preferences);
    preferences->setMenuRole(QAction::PreferencesRole);
    auto* view = menuBar()->addMenu(tr("&View"));
#ifdef Q_OS_MACOS
    const QKeySequence filteredShortcut("Ctrl+Shift+H");
#else
    const QKeySequence filteredShortcut("Ctrl+H");
#endif
    add(view, "filtered-only", tr("Show Only &Filtered Lines"), filteredShortcut, true);
    add(view, "line-numbers", tr("Line &numbers"), {}, true, true);
    add(view, "filter-tooltips", tr("Filter tooltips"), {}, true, true);
    auto* markerMenu = view->addMenu(tr("Marker gutter"));
    auto* markerGroup = new QActionGroup(this);
    markerGroup->setExclusive(true);
    markerGroup->addAction(add(markerMenu, "markers-always", tr("Always"), {}, true));
    markerGroup->addAction(add(markerMenu, "markers-never", tr("Never"), {}, true));
    markerGroup->addAction(add(markerMenu, "markers-in-use", tr("When in use"), {}, true, true));
    view->addSeparator();
    add(view, "zoom-in", tr("Zoom &in"), QKeySequence::ZoomIn);
    add(view, "zoom-out", tr("Zoom &out"), QKeySequence::ZoomOut);
    add(view, "zoom-reset", tr("Reset zoom"), QKeySequence("Ctrl+0"));
    auto* dockMenu = view->addMenu(tr("Filter dock"));
    add(dockMenu, "dock-left", tr("Left"));
    add(dockMenu, "dock-right", tr("Right"));
    add(dockMenu, "dock-top", tr("Top"));
    add(dockMenu, "dock-bottom", tr("Bottom"));
    view->addAction(impl_->dock->toggleViewAction());
    add(view, "encoding", tr("Encoding…"));
    auto* filters = menuBar()->addMenu(tr("F&ilters"));
    add(filters, "filter-previous", tr("Previous match"), QKeySequence("Shift+F8"))
        ->setShortcuts({QKeySequence("Shift+F8"), QKeySequence("Alt+Up")});
    add(filters, "filter-next", tr("Next match"), QKeySequence("F8"))
        ->setShortcuts({QKeySequence("F8"), QKeySequence("Alt+Down")});
    filters->addSeparator();
    add(filters, "add-filter", tr("&Add New Filter…"), QKeySequence::New);
    add(filters, "edit-filter", tr("&Edit filter…"));
    add(filters, "remove-filter", tr("&Remove selected filters"));
    filters->addSeparator();
    add(filters, "enable-all", tr("Enable all"));
    add(filters, "disable-all", tr("Disable all"));
    add(filters, "remove-all", tr("Remove all"));
    auto* help = menuBar()->addMenu(tr("&Help"));
    add(help, "documentation", tr("&Documentation"));
    add(help, "plugins", tr("&Plugins"));
    auto* about = add(help, "about", tr("&About TextAnalysisTool"));
    about->setMenuRole(QAction::AboutRole);
    connect(impl_->table, &QTableView::doubleClicked, this, [this] { emit commandRequested("edit-filter"); });
    impl_->table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(impl_->table, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        const auto index = impl_->table->indexAt(point);
        if (index.isValid() && !impl_->table->selectionModel()->isSelected(index))
            impl_->table->selectRow(index.row());
        QMenu menu(this);
        for (const auto& command : {"add-filter", "edit-filter", "remove-filter", "copy-filters", "paste-filters",
            "filter-previous", "filter-next", "enable-all", "disable-all", "remove-all"})
            menu.addAction(findChild<QAction*>(command));
        menu.exec(impl_->table->viewport()->mapToGlobal(point));
    });
    impl_->status = new QLabel(this);
    impl_->status->setObjectName("sourceStatus");
    impl_->status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    impl_->activity = new QLabel(this);
    impl_->activity->setObjectName("activityStatus");
    impl_->cancel = new StatusCancelButton(tr("Cancel"), this);
    impl_->cancel->setFlat(true);
    impl_->cancel->setObjectName("cancel");
    impl_->cancel->setAccessibleName(tr("Cancel current operation"));
    impl_->cancelAction = new QAction(tr("Cancel current operation"), this);
    impl_->cancelAction->setObjectName("cancel");
    impl_->cancelAction->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(impl_->cancelAction);
    connect(impl_->cancelAction, &QAction::triggered, this, [this] { emit commandRequested("cancel"); });
    connect(impl_->cancel, &QPushButton::clicked, impl_->cancelAction, &QAction::trigger);
    statusBar()->addWidget(impl_->status, 1);
    statusBar()->addPermanentWidget(impl_->activity);
    statusBar()->addPermanentWidget(impl_->cancel);
    setBusy(false);
    setRecentFiles({}, {});
    setStatus({}, 0, 0, 0, 100);
    connect(impl_->log, &LogView::zoomChanged, this, [this](int percent) {
        setStatus(impl_->statusSource, impl_->statusSelected, impl_->statusFiltered,
            impl_->statusTotal, percent, impl_->activity->text());
    });
}
MainWindow::~MainWindow() = default;
LogView* MainWindow::logView() const { return impl_->log; }
FilterModel* MainWindow::filterModel() const { return impl_->model; }
QTableView* MainWindow::filterView() const { return impl_->table; }
QDockWidget* MainWindow::filterDock() const { return impl_->dock; }
void MainWindow::setStatus(QString source, LineId selected, LineId filtered, LineId total,
    int zoom, QString activity) {
    impl_->statusSource = source;
    impl_->statusSelected = selected;
    impl_->statusFiltered = filtered;
    impl_->statusTotal = total;
    impl_->status->setText(tr("%1   Sel %2   Fil %3   Total %4   %5%")
        .arg(source).arg(qulonglong(selected)).arg(qulonglong(filtered)).arg(qulonglong(total)).arg(zoom));
    impl_->status->setAccessibleName(impl_->status->text());
    impl_->activity->setText(activity);
}
void MainWindow::setRecentFiles(const QStringList& files, const QStringList& filters) {
    auto populate = [this](QMenu* menu, const QStringList& paths, const QString& prefix, const QString& clear) {
        menu->clear();
        for (const auto& path : paths) {
            QString label = path;
            label.replace("&", "&&");
            auto* action = menu->addAction(label);
            action->setObjectName(prefix + path);
            connect(action, &QAction::triggered, this, [this, prefix, path] { emit commandRequested(prefix + path); });
        }
        menu->addSeparator();
        auto* action = menu->addAction(tr("Clear recent list"));
        action->setObjectName(clear);
        action->setEnabled(!paths.isEmpty());
        connect(action, &QAction::triggered, this, [this, clear] { emit commandRequested(clear); });
    };
    populate(impl_->recentFiles, files, "open-recent:", "clear-recent-files");
    populate(impl_->recentFilters, filters, "filters-recent:", "clear-recent-filters");
}
void MainWindow::setBusy(bool busy) {
    impl_->cancel->setVisible(busy);
    impl_->cancel->setEnabled(busy);
    impl_->cancelAction->setEnabled(busy);
    impl_->activity->setAccessibleDescription(busy ? tr("Operation in progress") : tr("Ready"));
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() || event->mimeData()->hasText()) event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent* event) {
    bool handled = false;
    if (event->mimeData()->hasUrls()) {
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile()) { emit pathDropped(url.toLocalFile()); handled = true; }
    } else if (event->mimeData()->hasText()) {
        emit textDropped(event->mimeData()->text());
        handled = true;
    }
    if (handled) event->acceptProposedAction();
}
void MainWindow::closeEvent(QCloseEvent* event) {
    bool accepted = true;
    emit closeRequested(&accepted);
    if (accepted) event->accept(); else event->ignore();
}
}
