#include "session.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFontDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextCodec>
#include <QUuid>
#include <QLineEdit>
#include <QtConcurrent>
#include <algorithm>
#include <stdexcept>

namespace textscope {
namespace {
const QString filterMime = "application/x-textscope-filters";
const QString legacyFilterMime = "application/x-tat-filters";
const QString windowsFilterMime = "application/x-qt-windows-mime;value=\"TEXTANALYSISTOOL.NET_FILTERS\"";

template<typename T> struct TaskResult {
    std::optional<T> value;
    QString error;
};

struct ClipboardPayload {
    QString text;
    QString html;
};

template<typename T, typename Work, typename Done>
void runAsync(QObject* owner, Work work, Done done)
{
    auto* watcher = new QFutureWatcher<TaskResult<T>>(owner);
    QObject::connect(watcher, &QFutureWatcher<TaskResult<T>>::finished, owner,
        [watcher, done = std::move(done)]() mutable {
            auto result = watcher->result();
            watcher->deleteLater();
            done(std::move(result));
        });
    watcher->setFuture(QtConcurrent::run([work = std::move(work)]() mutable {
        TaskResult<T> result;
        try {
            result.value.emplace(work());
        } catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }

        return result;
    }));
}

CancelFlag newCancel() { return std::make_shared<std::atomic_bool>(false); }
void cancel(const CancelFlag& flag) { if (flag) flag->store(true); }

QByteArray filterBytes(const QMimeData* data)
{
    if (data->hasFormat(filterMime)) return data->data(filterMime);
    if (data->hasFormat(legacyFilterMime)) return data->data(legacyFilterMime);
    if (data->hasFormat(windowsFilterMime)) return data->data(windowsFilterMime);
    if (data->hasFormat("TEXTANALYSISTOOL.NET_FILTERS")) return data->data("TEXTANALYSISTOOL.NET_FILTERS");
    return {};
}

QString sourceName(const QString& path)
{
    return path.isEmpty() ? QString("Clipboard") : path;
}
}

Session::Session(const Startup& startup, QObject* parent)
    : QObject(parent), window_(new MainWindow), settings_(),
      preferences_(loadPreferences(settings_)), plugins_(std::make_shared<Plugins>()),
      direct_(startup.direct), startup_(startup)
{
    window_->setAttribute(Qt::WA_DeleteOnClose);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &Session::cancelJobs);
    connect(window_, &QObject::destroyed, this, [this] { window_ = nullptr; deleteLater(); });
    plugins_->discover(QCoreApplication::applicationDirPath() + "/plugins");
    plugins_->discover(QCoreApplication::applicationDirPath() + "/../PlugIns/textscope");
    history_ = settings_.value("patternHistory").toStringList();
    findQuery_.id = "find";
    filterTimer_.setSingleShot(true);
    filterTimer_.setInterval(80);
    connect(&filterTimer_, &QTimer::timeout, this, &Session::changeFilters);
    connect(window_, &MainWindow::commandRequested, this, &Session::command);
    connect(window_, &MainWindow::pathDropped, this, [this](const QString& path) {
        try {
            if (path.endsWith(".tat", Qt::CaseInsensitive)) loadFilterFile(path, false);
            else if (confirmReplacement()) open(path);
        } catch (const std::exception& error) {
            showError(QString::fromUtf8(error.what()));
        }
    });
    connect(window_, &MainWindow::textDropped, this, [this](const QString& text) { pasteText(text); });
    connect(window_, &MainWindow::closeRequested, this, [this](bool* accepted) {
        *accepted = confirmDirty();
        if (!*accepted) return;
        cancelJobs();
        try {
            savePreferences(settings_, preferences_);
            settings_.setValue("geometry", window_->saveGeometry());
            if (preferences_.rememberLayout) {
                settings_.setValue("windowState", window_->saveState());
                settings_.setValue("compactFilterHeader", window_->filterView()->horizontalHeader()->saveState());
            }
            settings_.setValue("patternHistory", history_);
            settings_.sync();
            if (settings_.status() != QSettings::NoError)
                throw std::runtime_error("Unable to save window settings");
        } catch (const std::exception& error) {
            showError(QString::fromUtf8(error.what()));
            *accepted = false;
        }
    });
    connect(window_->filterModel(), &FilterModel::filtersChanged, this, [this] {
        if (updatingModel_) return;
        filters_ = window_->filterModel()->filters();
        dirty_ = dirty_ || preferences_.markToggleChanges;
        staleAnalysis_ = static_cast<bool>(snapshot_);
        if (staleAnalysis_ && !loading_) activity_ = "Filter changes pending...";
        filterTimer_.start();
        updateStatus();
    });
    auto* view = window_->logView();
    connect(view, &LogView::zoomChanged, this, [this](int percent) {
        if (preferences_.zoom == percent) return;
        preferences_.zoom = percent;
        applyPreferences();
    });
    connect(view, &LogView::selectionChanged, this, &Session::updateStatus);
    connect(view, &LogView::errorOccurred, this, &Session::showError);
    connect(view, &LogView::toggleMarkerRequested, this, &Session::toggleMarker);
    connect(view, &LogView::navigateRequested, this, &Session::navigate);
    connect(view, &LogView::markerNavigationRequested, this, &Session::navigateMarker);
    connect(view, &LogView::addFilterRequested, this, &Session::addFilter);
    connect(view, &LogView::toggleFilterRequested, this, [this](int index) {
        if (index < 0 || index >= static_cast<int>(filters_.size())) return;
        filters_[index].enabled = !filters_[index].enabled;
        dirty_ = dirty_ || preferences_.markToggleChanges;
        refresh();
        changeFilters();
    });
    applyPreferences();
    window_->restoreGeometry(settings_.value("geometry").toByteArray());
    if (preferences_.rememberLayout) {
        window_->restoreState(settings_.value("windowState").toByteArray());
        window_->filterView()->horizontalHeader()->restoreState(settings_.value("compactFilterHeader").toByteArray());
    }
    window_->setRecentFiles(recentFiles(settings_, "recentFiles"), recentFiles(settings_, "recentFilters"));
    window_->show();
    QTimer::singleShot(0, this, [this] {
        try {
            if (!startup_.configuration.isEmpty())
                preferences_ = readPreferences(readSmallFile(startup_.configuration), preferences_);
            if (!startup_.encoding.isEmpty()) preferences_.encoding = startup_.encoding;
            applyPreferences();
            for (qsizetype i = 0; i < startup_.filters.size(); ++i)
                loadFilterFile(startup_.filters[i], i > 0);
            if (startup_.clipboard) pasteText(QApplication::clipboard()->text(), false);
            else if (!startup_.input.isEmpty()) open(startup_.input);
            else updateStatus();
        } catch (const std::exception& error) {
            showError(QString::fromUtf8(error.what()));
        }
    });
}

Session::~Session()
{
    cancelJobs();
    if (window_) {
        window_->disconnect(this);
        delete window_;
    }
}
MainWindow* Session::window() const { return window_; }
void Session::chooseFile()
{
    if (openDialog_) {
        openDialog_->raise();
        openDialog_->activateWindow();
        return;
    }
    auto* dialog = new QFileDialog(window_, "Open Text File",
        sourcePath_.isEmpty() ? QString() : QFileInfo(sourcePath_).absolutePath(),
        "Text and log files (*.txt *.log);;All files (*)");
    dialog->setObjectName("openFileDialog");
#ifdef Q_OS_MACOS
    // Use the Qt chooser: selection/cancellation must finish through the same window-modal lifecycle.
    dialog->setOption(QFileDialog::DontUseNativeDialog);
#endif
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    openDialog_ = dialog;
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& path) {
        if (confirmReplacement()) open(path);
    });
    // Keep the application event loop running while the chooser is open.
    dialog->open();
}
std::shared_ptr<const Snapshot> Session::document() const { return snapshot_; }
const Analysis& Session::analysis() const { return analysis_; }
const Markers& Session::markers() const { return markers_; }
bool Session::busy() const { return pendingJobs_ > 0 || filterTimer_.isActive(); }

QAction* Session::action(const QString& name) const
{
    return window_->findChild<QAction*>(name);
}

void Session::showError(const QString& message)
{
    qWarning().noquote() << message;
    if (window_) QMessageBox::critical(window_, "TextScope", message);
}

void Session::beginTask(const QString& activity)
{
    ++pendingJobs_;
    activity_ = activity;
    window_->setBusy(true);
    updateStatus();
}

void Session::endTask()
{
    pendingJobs_ = qMax(0, pendingJobs_ - 1);
    if (pendingJobs_ == 0) {
        activity_ = staleAnalysis_ ? "Previous completed view; filter changes not applied." : QString();
        if (window_) window_->setBusy(false);
    }
    updateStatus();
}

void Session::cancelJobs()
{
    cancel(loadCancel_);
    cancel(analysisCancel_);
    cancel(auxiliaryCancel_);
    cancel(markerCancel_);
    markerTasks_.clear();
    ++loadRevision_;
    ++analysisRevision_;
    filterTimer_.stop();
    setLoading(false);
    activity_ = pendingJobs_ ? "Cancelling..." : staleAnalysis_
        ? "Previous completed view; filter changes not applied." : QString();
    if (window_) window_->logView()->clearLoadingPreview();
    updateStatus();
}

void Session::open(const QString& path)
{
    cancel(loadCancel_);
    cancel(analysisCancel_);
    cancel(auxiliaryCancel_);
    cancel(markerCancel_);
    markerTasks_.clear();
    const auto revision = ++loadRevision_;
    ++analysisRevision_;
    auto flag = loadCancel_ = newCancel();
    OpenOptions options;
    options.encoding = preferences_.encoding;
    options.direct = direct_;
    const auto plugins = plugins_;
    setLoading(true);
    beginTask(direct_ ? "Opening direct-file view..." : "Capturing immutable snapshot...");
    window_->logView()->setLoadingPreview("Loading source...");
    auto* feedback = new LoadFeedback(qApp);
    connect(feedback, &LoadFeedback::previewReady, this, [this, revision](const QString& text) {
        if (revision == loadRevision_) {
            window_->logView()->setLoadingPreview(text);
            emit previewAvailable();
        }
    });
    connect(feedback, &LoadFeedback::progressReady, this, [this, revision](qulonglong done, qulonglong total) {
        if (revision != loadRevision_) return;
        activity_ = QString("%1 %2%")
            .arg(direct_ ? "Indexing direct file..." : "Capturing snapshot...")
            .arg(total ? static_cast<int>(100.0L * done / total) : 0);
        updateStatus();
    });
    runAsync<std::shared_ptr<const Snapshot>>(this,
        [plugins, path, options, flag, feedback] {
            if (!flag->load()) {
                QFile input(path);
                if (!input.open(QIODevice::ReadOnly))
                    throw std::runtime_error(input.errorString().toStdString());
                const auto prefix = input.read(64 * 1024);
                if (input.error() != QFileDevice::NoError)
                    throw std::runtime_error(input.errorString().toStdString());
                if (!plugins->needsParsing(path, prefix)) {
                    auto* codec = QTextCodec::codecForName(options.encoding);
                    if (!codec) throw std::runtime_error("Unsupported preview encoding");
                    codec = QTextCodec::codecForUtfText(prefix, codec);
                    emit feedback->previewReady(codec->toUnicode(prefix));
                }
            }
            QElapsedTimer elapsed;
            elapsed.start();
            return plugins->open(path, options, flag, [feedback, &elapsed](std::uint64_t done, std::uint64_t total) {
                if (elapsed.elapsed() >= 75 || done == total) {
                    emit feedback->progressReady(done, total);
                    elapsed.restart();
                }
            });
        },
        [this, revision, flag, path, feedback](TaskResult<std::shared_ptr<const Snapshot>> result) {
            feedback->deleteLater();
            endTask();
            if (revision != loadRevision_ || flag->load()) return;
            setLoading(false);
            if (!result.value) {
                window_->logView()->clearLoadingPreview();
                showError(result.error);
                return;
            }
            afterLoad(std::move(*result.value), path);
        });
}

void Session::afterLoad(std::shared_ptr<const Snapshot> snapshot, const QString& path)
{
    snapshot_ = std::move(snapshot);
    sourcePath_ = path;
    markers_ = {};
    cache_.clear();
    analysis_ = {};
    staleAnalysis_ = false;
    publishedFilters_.clear();
    publishedMarkers_ = {};
    analysis_.accepted = LineSet::all(snapshot_->lineCount());
    refresh();
    if (!path.isEmpty()) rememberFile(settings_, "recentFiles", path);
    changeFilters();
    if (startup_.line > 0) {
        if (startup_.line <= snapshot_->lineCount())
            window_->logView()->goToLine(startup_.line - 1);
        else showError("The requested startup line is outside the document.");
        startup_.line = 0;
    }
}

bool Session::confirmReplacement()
{
    if (!snapshot_ || !preferences_.confirmReplacement) return true;
    return QMessageBox::question(window_, "Replace text",
        "Replace the text currently being analyzed?", QMessageBox::Yes | QMessageBox::No)
        == QMessageBox::Yes;
}

void Session::pasteText(const QString& text, bool confirm)
{
    if (confirm && !confirmReplacement()) return;
    cancel(loadCancel_);
    cancel(analysisCancel_);
    cancel(auxiliaryCancel_);
    cancel(markerCancel_);
    markerTasks_.clear();
    const auto revision = ++loadRevision_;
    ++analysisRevision_;
    const auto flag = loadCancel_ = newCancel();
    setLoading(true);
    beginTask("Loading clipboard text...");
    runAsync<std::shared_ptr<const Snapshot>>(this,
        [text, flag] { return Snapshot::fromText(text, flag); },
        [this, revision, flag](TaskResult<std::shared_ptr<const Snapshot>> result) {
            endTask();
            if (revision != loadRevision_ || flag->load()) return;
            setLoading(false);
            if (!result.value) { showError(result.error); return; }
            afterLoad(std::move(*result.value), {});
        });
}

void Session::changeFilters()
{
    filterTimer_.stop();
    staleAnalysis_ = static_cast<bool>(snapshot_);
    if (loading_) { refresh(); return; }
    if (!snapshot_) { refresh(); return; }
    cancel(analysisCancel_);
    const auto revision = ++analysisRevision_;
    const auto flag = analysisCancel_ = newCancel();
    const auto snapshot = snapshot_;
    const auto filters = filters_;
    const auto markers = markers_;
    std::vector<std::optional<LineSet>> cached;
    cached.reserve(filters.size());
    for (const auto& filter : filters) {
        const auto key = predicateKey(filter);
        const auto it = cache_.constFind(key);
        cached.emplace_back(filter.kind != Filter::Kind::Marker && it != cache_.cend()
            ? std::optional<LineSet>(*it) : std::nullopt);
    }
    beginTask("Filtering...");
    runAsync<Analysis>(this,
        [snapshot, filters, markers, cached, flag] { return analyze(snapshot, filters, markers, cached, flag, {}); },
        [this, revision, flag, filters, markers](TaskResult<Analysis> result) {
            endTask();
            if (revision != analysisRevision_ || flag->load()) return;
            if (!result.value) { showError(result.error); return; }
            analysis_ = std::move(*result.value);
            staleAnalysis_ = false;
            if (pendingJobs_ == 0) activity_.clear();
            publishedFilters_ = filters;
            publishedMarkers_ = markers;
            cache_.clear();
            for (std::size_t i = 0; i < filters.size(); ++i)
                if (filters[i].kind != Filter::Kind::Marker)
                    cache_.insert(predicateKey(filters[i]), analysis_.matches[i]);
            refresh();
        });
}

void Session::refresh()
{
    updatingModel_ = true;
    window_->filterModel()->setFilters(filters_);
    bool samePredicates = filters_.size() == publishedFilters_.size();
    for (std::size_t i = 0; samePredicates && i < filters_.size(); ++i)
        samePredicates = predicateKey(filters_[i]) == predicateKey(publishedFilters_[i]);
    window_->filterModel()->setCounts(samePredicates ? analysis_.matches : std::vector<LineSet>{},
        snapshot_ ? snapshot_->lineCount() : 0);
    updatingModel_ = false;
    if (snapshot_ && !loading_) window_->logView()->setDocument(snapshot_, publishedFilters_, analysis_, publishedMarkers_);
    window_->setRecentFiles(recentFiles(settings_, "recentFiles"), recentFiles(settings_, "recentFilters"));
    updateStatus();
}

void Session::updateStatus()
{
    if (!window_) return;
    const auto source = snapshot_ ? sourceName(sourcePath_) : "No document";
    const auto titleSource = sourcePath_.isEmpty() ? source
        : preferences_.fullPathTitle ? sourcePath_ : QFileInfo(sourcePath_).fileName();
    const auto filterTitle = filterPath_.isEmpty() ? QString() : " — " + QFileInfo(filterPath_).fileName();
    window_->setWindowTitle(titleSource + filterTitle + (dirty_ ? " *" : "") + " — TextScope");
    window_->setStatus(source + (snapshot_ && snapshot_->isDirect() ? " [DIRECT]" : ""),
        window_->logView()->selection().count(), analysis_.accepted.count(),
        snapshot_ ? snapshot_->lineCount() : 0, window_->logView()->zoom(), activity_);
}

FilterFile Session::filterFile() const
{
    return {filters_, window_->logView()->filteredOnly(), filterAttributes_};
}

bool Session::confirmDirty()
{
    if (!dirty_ || filterPath_.isEmpty()) return true;
    const auto answer = QMessageBox::question(window_, "Save filters",
        "Save changes to the current filter file?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) return false;
    return answer != QMessageBox::Save || saveFilterFile(false);
}

bool Session::saveFilterFile(bool choosePath)
{
    QString path = filterPath_;
    if (choosePath || path.isEmpty())
        path = QFileDialog::getSaveFileName(window_, "Save Filters", path, "Filter files (*.tat)");
    if (path.isEmpty()) return false;
    try {
        if (!path.endsWith(".tat", Qt::CaseInsensitive)) path += ".tat";
        saveFilters(path, filterFile());
        filterPath_ = path;
        dirty_ = false;
        rememberFile(settings_, "recentFilters", path);
        refresh();
        return true;
    } catch (const std::exception& error) {
        showError(QString::fromUtf8(error.what()));
        return false;
    }
}

void Session::loadFilterFile(const QString& path, bool append)
{
    if (!append && !confirmDirty()) return;
    const auto loaded = loadFilters(path);
    if (!append) {
        filters_ = loaded.filters;
        filterAttributes_ = loaded.attributes;
        filterPath_ = path;
        dirty_ = false;
    } else {
        filters_.insert(filters_.end(), loaded.filters.begin(), loaded.filters.end());
        dirty_ = true;
    }
    window_->logView()->setFilteredOnly(loaded.filteredOnly);
    if (auto* a = action("filtered-only")) a->setChecked(loaded.filteredOnly);
    rememberFile(settings_, "recentFilters", path);
    refresh();
    changeFilters();
}

std::vector<int> Session::selectedFilters() const
{
    std::vector<int> indexes;
    for (const auto& index : window_->filterView()->selectionModel()->selectedRows())
        indexes.push_back(index.row());
    std::sort(indexes.begin(), indexes.end());
    return indexes;
}

void Session::rememberHistory(const QString& pattern)
{
    if (pattern.isEmpty()) return;
    history_.removeAll(pattern);
    history_.prepend(pattern);
    while (history_.size() > 50) history_.removeLast();
}

void Session::addFilter(const QString& pattern)
{
    Filter filter;
    filter.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    filter.pattern = pattern;
    const auto edited = editFilter(window_, filter, history_, false, true);
    if (!edited) return;
    filters_.push_back(*edited);
    rememberHistory(edited->pattern);
    dirty_ = true;
    refresh();
    changeFilters();
}

void Session::editSelectedFilters()
{
    const auto selected = selectedFilters();
    if (selected.empty()) return;
    const auto original = filters_[selected.front()];
    const auto edited = editFilter(window_, original, history_);
    if (!edited) return;
    if (selected.size() == 1) {
        filters_[selected.front()] = *edited;
    } else {
        auto changed = filters_;
        for (const int index : selected) {
            auto& filter = changed[index];
            if (edited->kind != original.kind) filter.kind = edited->kind;
            if (edited->pattern != original.pattern) filter.pattern = edited->pattern;
            if (edited->description != original.description) filter.description = edited->description;
            if (edited->marker != original.marker) filter.marker = edited->marker;
            if (edited->enabled != original.enabled) filter.enabled = edited->enabled;
            if (edited->excluding != original.excluding) filter.excluding = edited->excluding;
            if (edited->caseSensitive != original.caseSensitive) filter.caseSensitive = edited->caseSensitive;
            if (edited->regularExpression != original.regularExpression) filter.regularExpression = edited->regularExpression;
            if (edited->foreground != original.foreground) filter.foreground = edited->foreground;
            if (edited->background != original.background) filter.background = edited->background;
            validateFilter(filter);
        }
        filters_ = std::move(changed);
    }
    rememberHistory(edited->pattern);
    dirty_ = true;
    refresh();
    changeFilters();
}

void Session::copyFilters()
{
    FilterFile file;
    for (const auto index : selectedFilters()) file.filters.push_back(filters_[index]);
    if (file.filters.empty()) return;
    auto* data = new QMimeData;
    const auto xml = writeFilters(file);
    data->setData(filterMime, xml);
    data->setData(legacyFilterMime, xml);
    data->setData(windowsFilterMime, xml);
    data->setText(QString::fromUtf8(xml));
    QApplication::clipboard()->setMimeData(data);
}

void Session::pasteFilters()
{
    auto bytes = filterBytes(QApplication::clipboard()->mimeData());
    if (bytes.isEmpty()) bytes = QApplication::clipboard()->text().toUtf8();
    const auto file = readFilters(bytes);
    filters_.insert(filters_.end(), file.filters.begin(), file.filters.end());
    dirty_ = true;
    refresh();
    changeFilters();
}

void Session::copyLines()
{
    if (!snapshot_ || loading_) return;
    const auto selection = window_->logView()->selection();
    if (!selection.count()) return;
    if (selection.count() > 100000 &&
        QMessageBox::question(window_, "Large clipboard copy",
            "This copy may use substantial memory. Continue? Use Save Current Lines for streaming output.")
            != QMessageBox::Yes) return;
    cancel(auxiliaryCancel_);
    const auto flag = auxiliaryCancel_ = newCancel();
    const auto snapshot = snapshot_;
    const bool lineNumbers = preferences_.copyLineNumbers;
    const auto filters = publishedFilters_;
    const auto matches = analysis_.matches;
    const auto textColor = preferences_.textColor.name();
    const auto background = preferences_.windowColor.name();
    beginTask("Preparing clipboard...");
    runAsync<ClipboardPayload>(this, [selection, snapshot, flag, lineNumbers, filters, matches, textColor, background] {
        ClipboardPayload result;
        result.html = QString("<pre style=\"font-family:monospace;white-space:pre;color:%1;background-color:%2\">")
            .arg(textColor, background);
        for (LineId rank = 0; rank < selection.count(); ++rank) {
            if (flag->load()) throw std::runtime_error("Cancelled");
            const auto id = selection.at(rank);
            QString line;
            if (lineNumbers) line = QString::number(id + 1) + ' ';
            line += snapshot->line(id);
            if (rank) { result.text += '\n'; result.html += '\n'; }
            result.text += line;
            QString style;
            for (std::size_t i = 0; i < filters.size() && i < matches.size(); ++i) {
                const auto& filter = filters[i];
                if (!filter.enabled || filter.excluding || (!filter.foreground && !filter.background) ||
                    !matches[i].contains(id)) continue;
                if (filter.foreground) style += "color:" + filter.foreground->name() + ';';
                if (filter.background) style += "background-color:" + filter.background->name() + ';';
                break;
            }
            const auto escaped = line.toHtmlEscaped();
            result.html += style.isEmpty() ? escaped : "<span style=\"" + style + "\">" + escaped + "</span>";
            if (result.text.size() + result.html.size() > 256 * 1024 * 1024)
                throw std::runtime_error("Clipboard copy exceeds 512 MiB. Use Save Current Lines instead.");
        }
        result.html += "</pre>";
        return result;
    }, [this, flag](TaskResult<ClipboardPayload> result) {
        endTask();
        if (flag->load()) return;
        if (!result.value) { showError(result.error); return; }
        auto* mime = new QMimeData;
        mime->setText(result.value->text);
        mime->setHtml(result.value->html);
        QApplication::clipboard()->setMimeData(mime);
    });
}

void Session::saveLines()
{
    if (!snapshot_ || loading_) return;
    const auto path = QFileDialog::getSaveFileName(window_, "Save Current Lines", {}, "Text files (*.txt);;All files (*)");
    if (path.isEmpty()) return;
    const auto snapshot = snapshot_;
    const auto visible = window_->logView()->filteredOnly()
        ? analysis_.accepted : LineSet::all(snapshot->lineCount());
    cancel(auxiliaryCancel_);
    const auto flag = auxiliaryCancel_ = newCancel();
    beginTask("Saving current lines as UTF-8...");
    runAsync<QString>(this, [snapshot, visible, path, flag] {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
        for (LineId rank = 0; rank < visible.count(); ++rank) {
            if (flag->load()) throw std::runtime_error("Cancelled");
            auto bytes = snapshot->line(visible.at(rank)).toUtf8();
            bytes += '\n';
            if (file.write(bytes) != bytes.size()) throw std::runtime_error(file.errorString().toStdString());
        }
        if (!file.commit()) throw std::runtime_error(file.errorString().toStdString());
        return path;
    }, [this, flag](TaskResult<QString> result) {
        endTask();
        if (flag->load()) return;
        if (!result.value) showError(result.error);
    });
}

void Session::findText(bool edit, bool forward)
{
    if (!snapshot_ || loading_) return;
    if (edit || !hasFind_) {
        const auto query = editFilter(window_, findQuery_, history_, true);
        if (!query) return;
        findQuery_ = *query;
        hasFind_ = true;
        rememberHistory(findQuery_.pattern);
    }
    cancel(auxiliaryCancel_);
    const auto flag = auxiliaryCancel_ = newCancel();
    const auto snapshot = snapshot_;
    const auto query = findQuery_;
    const auto current = window_->logView()->currentLine();
    beginTask("Finding...");
    runAsync<std::optional<LineId>>(this,
        [snapshot, query, current, forward, flag] { return find(snapshot, query, current, forward, true, flag); },
        [this, flag](TaskResult<std::optional<LineId>> result) {
            endTask();
            if (flag->load()) return;
            if (!result.value) { showError(result.error); return; }
            if (!*result.value) {
                QMessageBox::information(window_, "Find", "No matching line.");
                return;
            }
            window_->logView()->goToLine(**result.value);
        });
}

void Session::navigate(int filter, bool forward)
{
    if (!snapshot_ || loading_) return;
    if (filter >= static_cast<int>(analysis_.matches.size())) return;
    try {
        const auto& matches = filter < 0 ? analysis_.accepted : analysis_.matches[filter];
        const auto next = matches.next(window_->logView()->currentLine(), forward);
        if (next) window_->logView()->goToLine(*next);
    } catch (const std::exception& error) {
        showError(QString::fromUtf8(error.what()));
    }
}

void Session::navigateMarker(int marker, bool forward)
{
    if (loading_ || marker < 0 || marker >= 8) return;
    try {
        const auto next = markers_[marker].next(window_->logView()->currentLine(), forward);
        if (next) window_->logView()->goToLine(*next);
    } catch (const std::exception& error) {
        showError(QString::fromUtf8(error.what()));
    }
}

void Session::toggleMarker(int marker)
{
    if (!snapshot_ || loading_ || marker < 0 || marker >= 8) return;
    const auto selected = window_->logView()->selection();
    if (!selected.count()) return;
    markerTasks_.emplace_back(marker, selected);
    if (!markerRunning_) runNextMarkerTask();
}

void Session::runNextMarkerTask()
{
    if (markerTasks_.empty()) return;
    const auto [marker, selected] = markerTasks_.front();
    markerTasks_.pop_front();
    markerRunning_ = true;
    const auto previous = markers_[marker];
    const auto source = snapshot_;
    const auto flag = markerCancel_ = newCancel();
    const auto revision = loadRevision_;
    beginTask("Updating markers...");
    runAsync<LineSet>(this, [previous, selected, source, flag] {
        LineSetBuilder builder;
        LineId a = 0, b = 0;
        while (a < previous.count() || b < selected.count()) {
            if (flag->load()) throw std::runtime_error("Cancelled");
            if (a == previous.count()) builder.add(selected.at(b++));
            else if (b == selected.count()) builder.add(previous.at(a++));
            else {
                const auto x = previous.at(a), y = selected.at(b);
                if (x < y) { builder.add(x); ++a; }
                else if (y < x) { builder.add(y); ++b; }
                else { ++a; ++b; }
            }
        }
        return builder.finish();
    }, [this, flag, revision, marker](TaskResult<LineSet> result) {
        markerRunning_ = false;
        endTask();
        if (flag->load() || revision != loadRevision_) {
            runNextMarkerTask();
            return;
        }
        if (!result.value) {
            markerTasks_.clear();
            showError(result.error);
            return;
        }
        markers_[marker] = std::move(*result.value);
        if (markerTasks_.empty()) changeFilters();
        else runNextMarkerTask();
    });
}

void Session::setLoading(bool loading)
{
    loading_ = loading;
    if (!window_) return;
    const QStringList commands{"reload", "save-lines", "copy", "select-all",
        "find", "find-next", "find-previous", "goto", "filter-next", "filter-previous"};
    for (const auto& name : commands)
        if (auto* a = action(name)) a->setEnabled(!loading);
}

void Session::applyPreferences()
{
    auto* view = window_->logView();
    view->setTextFont(preferences_.font);
    view->setZoom(preferences_.zoom);
    view->setTabWidth(preferences_.tabWidth);
    view->setMarkerVisibility(preferences_.markerVisibility);
    view->setLineNumbers(preferences_.lineNumbers);
    view->setFilterTooltips(preferences_.tooltips);
    QPalette palette = view->palette();
    palette.setColor(QPalette::Base, preferences_.windowColor);
    palette.setColor(QPalette::Text, preferences_.textColor);
    palette.setColor(QPalette::Highlight, preferences_.selectionColor);
    palette.setColor(QPalette::HighlightedText, preferences_.selectionTextColor);
    view->setPalette(palette);
    window_->filterView()->setPalette(palette);
    if (auto* a = action("line-numbers")) a->setChecked(preferences_.lineNumbers);
    if (auto* a = action("filter-tooltips")) a->setChecked(preferences_.tooltips);
    view->viewport()->update();
    updateStatus();
}

void Session::chooseEncoding()
{
    QStringList encodings{"System Default", "UTF-8", "UTF-16LE", "UTF-16BE", "Windows-1252", "GB18030"};
    for (const auto& encoding : QTextCodec::availableCodecs()) {
        const auto name = QString::fromLatin1(encoding);
        if (!encodings.contains(name)) encodings.append(name);
    }
    bool ok = false;
    const auto selected = QInputDialog::getItem(window_, "View Encoding",
        "BOM takes precedence. Changing this setting reloads the current file.",
        encodings, qMax(0, encodings.indexOf(QString::fromLatin1(preferences_.encoding))), false, &ok);
    if (!ok) return;
    preferences_.encoding = selected == "System Default" ? QTextCodec::codecForLocale()->name() : selected.toLatin1();
    if (!sourcePath_.isEmpty()) open(sourcePath_);
    else if (snapshot_) QMessageBox::information(window_, "Encoding", "Clipboard text is already Unicode.");
}

void Session::editPreferences()
{
    QDialog dialog(window_);
    dialog.setWindowTitle("Preferences");
    auto working = preferences_;
    auto* form = new QFormLayout(&dialog);
    auto* fontButton = new QPushButton(working.font.family());
    connect(fontButton, &QPushButton::clicked, &dialog, [&] {
        bool ok = false;
        auto selected = QFontDialog::getFont(&ok, working.font, &dialog);
        if (ok) { working.font = selected; fontButton->setText(selected.family()); }
    });
    form->addRow("Font:", fontButton);
    auto* tabs = new QSpinBox;
    tabs->setRange(1, 32);
    tabs->setValue(working.tabWidth);
    form->addRow("Tab width:", tabs);
    auto* encoding = new QComboBox;
    for (const auto& name : QTextCodec::availableCodecs()) encoding->addItem(QString::fromLatin1(name));
    encoding->setCurrentText(QString::fromLatin1(working.encoding));
    form->addRow("Default encoding:", encoding);
    auto checkbox = [form](const QString& label, bool initial) {
        auto* box = new QCheckBox;
        box->setChecked(initial);
        form->addRow(label, box);
        return box;
    };
    auto* copyNumbers = checkbox("Copy line numbers:", working.copyLineNumbers);
    auto* replacement = checkbox("Confirm replacing text:", working.confirmReplacement);
    auto* remember = checkbox("Remember column/dock layout:", working.rememberLayout);
    auto* fullTitle = checkbox("Full path in title:", working.fullPathTitle);
    auto* toggleDirty = checkbox("Mark enabled/reordered filters changed:", working.markToggleChanges);
    const auto colorButton = [&](const QString& label, QColor& value) {
        auto* button = new QPushButton(value.name());
        connect(button, &QPushButton::clicked, &dialog, [&value, button, &dialog] {
            const auto selected = QColorDialog::getColor(value, &dialog);
            if (selected.isValid()) { value = selected; button->setText(value.name()); }
        });
        form->addRow(label, button);
    };
    colorButton("Text color:", working.textColor);
    colorButton("Background:", working.windowColor);
    colorButton("Selection:", working.selectionColor);
    colorButton("Selected text:", working.selectionTextColor);
    auto* importButton = new QPushButton("Import XML...");
    auto* exportButton = new QPushButton("Export XML...");
    form->addRow(importButton, exportButton);
    connect(importButton, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Import Preferences", {}, "XML (*.xml)");
        if (path.isEmpty()) return;
        try {
            working = readPreferences(readSmallFile(path), working);
            tabs->setValue(working.tabWidth);
            encoding->setCurrentText(QString::fromLatin1(working.encoding));
            fontButton->setText(working.font.family());
            copyNumbers->setChecked(working.copyLineNumbers);
            replacement->setChecked(working.confirmReplacement);
            remember->setChecked(working.rememberLayout);
            fullTitle->setChecked(working.fullPathTitle);
            toggleDirty->setChecked(working.markToggleChanges);
        } catch (const std::exception& error) { showError(QString::fromUtf8(error.what())); }
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getSaveFileName(&dialog, "Export Preferences", {}, "XML (*.xml)");
        if (path.isEmpty()) return;
        try {
            working.tabWidth = tabs->value();
            working.encoding = encoding->currentText().toLatin1();
            working.copyLineNumbers = copyNumbers->isChecked();
            working.confirmReplacement = replacement->isChecked();
            working.rememberLayout = remember->isChecked();
            working.fullPathTitle = fullTitle->isChecked();
            working.markToggleChanges = toggleDirty->isChecked();
            writeFileAtomically(path, writePreferences(working));
        } catch (const std::exception& error) { showError(QString::fromUtf8(error.what())); }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    working.tabWidth = tabs->value();
    working.encoding = encoding->currentText().toLatin1();
    working.copyLineNumbers = copyNumbers->isChecked();
    working.confirmReplacement = replacement->isChecked();
    working.rememberLayout = remember->isChecked();
    working.fullPathTitle = fullTitle->isChecked();
    working.markToggleChanges = toggleDirty->isChecked();
    preferences_ = working;
    savePreferences(settings_, preferences_);
    applyPreferences();
}

void Session::command(const QString& name)
{
    try {
        if (name == "new-window") emit newWindowRequested();
        else if (name.startsWith("open-recent:")) {
            if (confirmReplacement()) open(name.mid(12));
        } else if (name.startsWith("filters-recent:")) loadFilterFile(name.mid(15), false);
        else if (name == "open") chooseFile();
        else if (name == "reload") {
            if (!sourcePath_.isEmpty()) open(sourcePath_);
        } else if (name == "save-lines") saveLines();
        else if (name == "load-filters" || name == "append-filters") {
            const auto path = QFileDialog::getOpenFileName(window_, "Load Filters", {}, "Filters (*.tat)");
            if (!path.isEmpty()) loadFilterFile(path, name == "append-filters");
        } else if (name == "save-filters") saveFilterFile(false);
        else if (name == "save-filters-as") saveFilterFile(true);
        else if (name == "clear-recent-files") {
            settings_.remove("recentFiles"); refresh();
        } else if (name == "clear-recent-filters") {
            settings_.remove("recentFilters"); refresh();
        } else if (name == "quit") QApplication::closeAllWindows();
        else if (name == "copy") {
            if (window_->filterView()->hasFocus()) copyFilters(); else copyLines();
        } else if (name == "paste") {
            if (!filterBytes(QApplication::clipboard()->mimeData()).isEmpty()) pasteFilters();
            else pasteText(QApplication::clipboard()->text());
        } else if (name == "copy-filters") copyFilters();
        else if (name == "paste-filters") pasteFilters();
        else if (name == "select-all") {
            if (window_->filterView()->hasFocus()) window_->filterView()->selectAll();
            else window_->logView()->selectAll();
        } else if (name == "find") findText(true, true);
        else if (name == "find-next") findText(false, true);
        else if (name == "find-previous") findText(false, false);
        else if (name == "goto") {
            if (!snapshot_ || !snapshot_->lineCount()) return;
            bool ok = false;
            const auto text = QInputDialog::getText(window_, "Go To", "Original line number (1-based):",
                QLineEdit::Normal, QString::number(window_->logView()->currentLine() + 1), &ok);
            if (!ok) return;
            bool number = false;
            const auto line = text.toULongLong(&number);
            if (!number || line == 0 || line > snapshot_->lineCount())
                throw std::runtime_error("Line number is outside the document");
            window_->logView()->goToLine(line - 1);
        } else if (name == "preferences") editPreferences();
        else if (name == "encoding") chooseEncoding();
        else if (name == "filtered-only") {
            window_->logView()->setFilteredOnly(action(name)->isChecked());
            dirty_ = dirty_ || preferences_.markToggleChanges;
            updateStatus();
        } else if (name == "line-numbers") {
            preferences_.lineNumbers = action(name)->isChecked(); applyPreferences();
        } else if (name == "filter-tooltips") {
            preferences_.tooltips = action(name)->isChecked(); applyPreferences();
        } else if (name.startsWith("markers-")) {
            preferences_.markerVisibility = name == "markers-always" ? 0 : name == "markers-never" ? 1 : 2;
            applyPreferences();
        } else if (name.startsWith("zoom-")) {
            preferences_.zoom = name == "zoom-reset" ? 100 : qBound(25,
                preferences_.zoom + (name == "zoom-in" ? 10 : -10), 400);
            applyPreferences();
        } else if (name.startsWith("dock-")) {
            const auto area = name == "dock-left" ? Qt::LeftDockWidgetArea
                : name == "dock-right" ? Qt::RightDockWidgetArea
                : name == "dock-top" ? Qt::TopDockWidgetArea : Qt::BottomDockWidgetArea;
            window_->addDockWidget(area, window_->filterDock());
        } else if (name == "add-filter") addFilter();
        else if (name == "edit-filter") editSelectedFilters();
        else if (name == "remove-filter") {
            const auto selected = selectedFilters();
            for (auto it = selected.rbegin(); it != selected.rend(); ++it) filters_.erase(filters_.begin() + *it);
            dirty_ = true; refresh(); changeFilters();
        } else if (name == "remove-all") {
            filters_.clear(); dirty_ = true; refresh(); changeFilters();
        } else if (name == "enable-all" || name == "disable-all") {
            for (auto& filter : filters_) filter.enabled = name == "enable-all";
            dirty_ = dirty_ || preferences_.markToggleChanges; refresh(); changeFilters();
        } else if (name == "filter-next" || name == "filter-previous") {
            const auto selected = selectedFilters();
            if (!selected.empty()) navigate(selected.front(), name == "filter-next");
        } else if (name == "cancel") cancelJobs();
        else if (name == "plugins") {
            auto lines = plugins_->descriptions();
            if (lines.isEmpty()) lines.append("No native parser plug-ins installed.");
            lines.append(plugins_->errors());
            lines.append("\nLegacy .NET assemblies require a managed adapter and are not loaded.");
            QMessageBox::information(window_, "Installed Plug-ins", lines.join('\n'));
            plugins_->configure(window_);
        } else if (name == "documentation") {
            QFile help(":/help.txt");
            if (!help.open(QIODevice::ReadOnly)) throw std::runtime_error("Bundled help cannot be read");
            pasteText(QString::fromUtf8(help.readAll()));
        } else if (name == "about") {
            QMessageBox::about(window_, "TextScope",
                "TextScope 0.1.0\nC++20 / Qt Widgets / PCRE2\n"
                "Immutable snapshots and large-file filtering.\n\n"
                "Native PCRE2 syntax is not identical to .NET regex.\n"
                "Original .tat attribute format is supported; original preferences XML and .NET plug-ins are not.\n"
                "Independent implementation; not affiliated with TextAnalysisTool.NET.");
        } else {
            throw std::runtime_error(("Unknown application command: " + name).toStdString());
        }
    } catch (const std::exception& error) {
        showError(QString::fromUtf8(error.what()));
    }
}
}
