#include "ui/ui.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDockWidget>
#include <QLabel>
#include <QHeaderView>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QScopeGuard>
#include <QStatusBar>
#include <QAbstractItemView>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QSignalSpy>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <limits>

using namespace textscope;

class WidgetsTest : public QObject {
    Q_OBJECT
private slots:
    void filterChecksCountsAndReorder() {
        FilterModel model;
        Filter a, b, c;
        a.pattern = "first"; b.pattern = "second"; c.pattern = "third";
        a.background = QColor(Qt::yellow);
        model.setFilters({a, b, c});
        model.setCounts({LineSet::all(7), LineSet::all(2)}, 10);
        QCOMPARE(model.columnCount(), 1);
        QVERIFY(model.data(model.index(0, 0)).toString().contains("a. Matches \"first\""));
        QVERIFY(model.data(model.index(0, 0)).toString().contains("[7 / 10]"));
        QCOMPARE(model.data(model.index(0, 0), Qt::BackgroundRole).value<QColor>(), QColor(Qt::yellow));
        QSignalSpy changed(&model, &FilterModel::filtersChanged);
        QVERIFY(model.setData(model.index(0, 0), Qt::Unchecked, Qt::CheckStateRole));
        QVERIFY(!model.filters()[0].enabled);
        QCOMPARE(changed.count(), 1);
        std::unique_ptr<QMimeData> mime(model.mimeData({model.index(0, 0), model.index(2, 0)}));
        QVERIFY(model.dropMimeData(mime.get(), Qt::MoveAction, 3, 0, {}));
        QCOMPARE(model.filters()[0].pattern, QString("second"));
        QCOMPARE(model.filters()[1].pattern, QString("first"));
        QCOMPARE(model.filters()[2].pattern, QString("third"));
        QVERIFY(model.data(model.index(1, 0)).toString().contains("[7 / 10]"));
        FilterModel foreign;
        foreign.setFilters({a});
        QVERIFY(!foreign.dropMimeData(mime.get(), Qt::MoveAction, 0, 0, {}));
        QTableView table;
        table.setModel(&model);
        table.selectRow(1);
        const auto selected = table.selectionModel()->selectedIndexes();
        auto edited = model.filters();
        edited[1].description = "new description";
        model.setFilters(edited);
        QCOMPARE(table.selectionModel()->selectedIndexes(), selected);
    }
    void menusDockStatusAndClose() {
        MainWindow window;
        QSignalSpy commands(&window, &MainWindow::commandRequested);
        QCOMPARE(window.dockWidgetArea(window.filterDock()), Qt::BottomDockWidgetArea);
        auto* open = window.findChild<QAction*>("open");
        QVERIFY(open);
        QCOMPARE(open->shortcut(), QKeySequence(QKeySequence::Open));
        auto* filtered = window.findChild<QAction*>("filtered-only");
        QVERIFY(filtered && filtered->isCheckable());
#ifdef Q_OS_MACOS
        QCOMPARE(filtered->shortcut(), QKeySequence("Ctrl+Shift+H"));
#else
        QCOMPARE(filtered->shortcut(), QKeySequence("Ctrl+H"));
#endif
        QCOMPARE(window.findChild<QAction*>("add-filter")->shortcut(), QKeySequence(QKeySequence::New));
        QCOMPARE(window.findChild<QAction*>("new-window")->shortcut(), QKeySequence("Ctrl+Shift+N"));
        QCOMPARE(window.findChild<QAction*>("filter-next")->shortcut(), QKeySequence("F8"));
        open->trigger();
        QCOMPARE(commands.takeFirst()[0].toString(), QString("open"));
        window.findChild<QAction*>("dock-left")->trigger();
        QCOMPARE(window.dockWidgetArea(window.filterDock()), Qt::LeftDockWidgetArea);
        window.setRecentFiles({"/path/a & b.log"}, {"/path/filters.json"});
        window.findChild<QAction*>("open-recent:/path/a & b.log")->trigger();
        QCOMPARE(commands.last()[0].toString(), QString("open-recent:/path/a & b.log"));
        window.setStatus("source", 2, 3, 10, 120, "Scanning");
        QVERIFY(window.findChild<QLabel*>("sourceStatus")->text().contains("Sel 2"));
        window.setBusy(true);
        QVERIFY(!window.findChild<QPushButton*>("cancel")->isHidden());
        window.setBusy(false);
        QVERIFY(window.findChild<QPushButton*>("cancel")->isHidden());
        auto connection = connect(&window, &MainWindow::closeRequested, &window, [](bool* accepted) { *accepted = false; });
        QVERIFY(!window.close());
        disconnect(connection);
        QVERIFY(window.close());
    }
    void dropsEmitPassiveRequests() {
        MainWindow window;
        QSignalSpy paths(&window, &MainWindow::pathDropped);
        QSignalSpy texts(&window, &MainWindow::textDropped);
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile("/path/a.log")});
        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &files, Qt::LeftButton, {});
        QApplication::sendEvent(&window, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &files, Qt::LeftButton, {});
        QApplication::sendEvent(&window, &drop);
        QCOMPARE(paths.first()[0].toString(), QString("/path/a.log"));
        QMimeData text;
        text.setText("dropped text");
        QDragEnterEvent textEnter(QPoint(20, 20), Qt::CopyAction, &text, Qt::LeftButton, {});
        QApplication::sendEvent(&window, &textEnter);
        QDropEvent textDrop(QPointF(20, 20), Qt::CopyAction, &text, Qt::LeftButton, {});
        QApplication::sendEvent(&window, &textDrop);
        QCOMPARE(texts.first()[0].toString(), QString("dropped text"));
    }
    void sourceSelectionAndFocusScopedKeys() {
        MainWindow window;
        auto* view = window.logView();
        const auto snapshot = Snapshot::fromText("zero\none\ntwo\nthree\nfour");
        Analysis analysis;
        LineSetBuilder accepted;
        accepted.add(1); accepted.add(3);
        analysis.accepted = accepted.finish();
        view->setDocument(snapshot, {}, analysis, {});
        view->setMarkerVisibility(1);
        view->goToLine(1);
        view->goToLine(3, true);
        QCOMPARE(view->selection().count(), LineId(3));
        view->setFilteredOnly(true);
        QVERIFY(view->selection().contains(2));
        view->goToLine(3, true);
        QCOMPARE(view->selection().count(), LineId(2));
        QVERIFY(view->selection().contains(1));
        view->selectAll();
        QCOMPARE(view->selection().count(), LineId(2));
        view->setDocument(snapshot, {}, analysis, {});
        view->goToLine(3, true);
        QVERIFY(view->selection().contains(1)); // Anchor survived the document analysis update.
        QSignalSpy navigate(view, &LogView::navigateRequested);
        QSignalSpy markers(view, &LogView::toggleMarkerRequested);
        QSignalSpy toggles(view, &LogView::toggleFilterRequested);
        QSignalSpy markerNavigation(view, &LogView::markerNavigationRequested);
        QTest::keyClick(view, Qt::Key_A);
        QCOMPARE(navigate.takeFirst()[0].toInt(), 0);
        QTest::keyClick(view, Qt::Key_Z, Qt::ShiftModifier);
        QVERIFY(!navigate.last()[1].toBool());
        QTest::keyClick(view, Qt::Key_3, commandModifier());
        QCOMPARE(markers.takeFirst()[0].toInt(), 2);
        QTest::keyClick(view, Qt::Key_B, commandModifier() | Qt::ShiftModifier);
        QCOMPARE(toggles.takeFirst()[0].toInt(), 1);
        QTest::keyClick(view, Qt::Key_8);
        QCOMPARE(markerNavigation.takeFirst()[0].toInt(), 7);
        const int before = navigate.count();
        QTest::keyClick(window.filterView(), Qt::Key_A);
        QCOMPARE(navigate.count(), before);
    }
    void additiveMouseSelection() {
        LogView view;
        view.resize(640, 300);
        auto snapshot = Snapshot::fromText("a\nb\nc\nd\ne");
        Analysis analysis;
        analysis.accepted = LineSet::all(snapshot->lineCount());
        view.setDocument(snapshot, {}, analysis, {});
        view.setMarkerVisibility(1);
        view.show();
        const int height = QFontMetrics(view.font()).height() + 4;
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, QPoint(200, height / 2));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, commandModifier(), QPoint(200, height * 2 + height / 2));
        QCOMPARE(view.selection().count(), LineId(2));
        QVERIFY(view.selection().contains(0) && view.selection().contains(2));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, commandModifier(), QPoint(200, height / 2));
        QCOMPARE(view.selection().count(), LineId(1));
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::ShiftModifier, QPoint(200, height * 4 + height / 2));
        QCOMPARE(view.selection().count(), LineId(5));
    }
    void shiftedSymbolsNavigatePreviousMarkers_data() {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("marker");
        const int keys[]{Qt::Key_Exclam, Qt::Key_At, Qt::Key_NumberSign, Qt::Key_Dollar,
            Qt::Key_Percent, Qt::Key_AsciiCircum, Qt::Key_Ampersand, Qt::Key_Asterisk};
        for (int marker = 0; marker < 8; ++marker)
            QTest::newRow(qPrintable(QString("shifted-marker-%1").arg(marker + 1))) << keys[marker] << marker;
    }
    void shiftedSymbolsNavigatePreviousMarkers() {
        QFETCH(int, key);
        QFETCH(int, marker);
        MainWindow window;
        auto* view = window.logView();
        QSignalSpy navigation(view, &LogView::markerNavigationRequested);
        QTest::keyClick(view, Qt::Key(key), Qt::ShiftModifier);
        QCOMPARE(navigation.count(), 1);
        QCOMPARE(navigation.first()[0].toInt(), marker);
        QVERIFY(!navigation.first()[1].toBool());
        QTest::keyClick(view, Qt::Key(Qt::Key_1 + marker), Qt::ShiftModifier);
        QCOMPARE(navigation.count(), 2);
        QVERIFY(!navigation.last()[1].toBool());
        QTest::keyClick(window.filterView(), Qt::Key(key), Qt::ShiftModifier);
        QCOMPARE(navigation.count(), 2);
    }
    void wheelZoomSynchronizesWithoutRecursiveSignals() {
        MainWindow window;
        auto* view = window.logView();
        int preferencesZoom = view->zoom();
        QSignalSpy changes(view, &LogView::zoomChanged);
        connect(view, &LogView::zoomChanged, &window, [&](int percent) {
            preferencesZoom = percent;
            window.filterView()->setFont(view->font());
            view->setZoom(percent);
        });
        QWheelEvent wheel(QPointF(50, 50), QPointF(50, 50), {}, QPoint(0, 120),
            Qt::NoButton, commandModifier(), Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &wheel);
        QCOMPARE(view->zoom(), 110);
        QCOMPARE(preferencesZoom, 110);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(window.filterView()->font(), view->font());
        QVERIFY(window.findChild<QLabel*>("sourceStatus")->text().contains("110%"));
        view->setZoom(110);
        QCOMPARE(changes.count(), 1);
        QWheelEvent end(QPointF(50, 50), QPointF(50, 50), {}, {},
            Qt::NoButton, commandModifier(), Qt::ScrollEnd, false);
        QApplication::sendEvent(view->viewport(), &end);
        QCOMPARE(view->zoom(), 110);
        QCOMPARE(changes.count(), 1);
        view->setZoom(999);
        QCOMPARE(preferencesZoom, 400);
        QCOMPARE(changes.count(), 2);
        view->setZoom(500);
        QCOMPARE(changes.count(), 2);
    }
    void sharedFontAndZoom_data() {
        QTest::addColumn<bool>("pixels");
        QTest::newRow("points") << false;
        QTest::newRow("pixels") << true;
    }
    void sharedFontAndZoom() {
        QFETCH(bool, pixels);
        MainWindow window;
        auto* view = window.logView();
        QFont font = view->font();
        font.setWeight(QFont::Medium);
        if (pixels) font.setPixelSize(16);
        else font.setPointSizeF(10);
        view->setTextFont(font);
        for (const int zoom : {100, 150, 200, 100}) {
            view->setZoom(zoom);
            QCOMPARE(window.filterView()->font(), view->font());
            QCOMPARE(view->font().weight(), QFont::Medium);
            if (pixels) QCOMPARE(view->font().pixelSize(), 16 * zoom / 100);
            else QCOMPARE(view->font().pointSizeF(), 10.0 * zoom / 100);
            QCOMPARE(window.filterView()->verticalHeader()->defaultSectionSize(),
                QFontMetrics(view->font()).height() + 4);
        }
        font.setWeight(QFont::Bold);
        view->setTextFont(font);
        QCOMPARE(view->font().weight(), QFont::Bold);
        QCOMPARE(window.filterView()->font(), view->font());
    }
    void compactStatusBar() {
        MainWindow window;
        window.show();
        window.setStatus("sample.log", 0, 1, 2, 100, "Reading");
        for (const bool busy : {false, true, false}) {
            window.setBusy(busy);
            QCoreApplication::processEvents();
            const auto* status = window.findChild<QLabel*>("sourceStatus");
            QVERIFY(window.statusBar()->height() >= status->fontMetrics().height());
            QVERIFY(window.statusBar()->height() <= status->fontMetrics().height() + 16);
            if (busy) {
                auto* cancel = window.findChild<QPushButton*>("cancel");
                QVERIFY(cancel->isVisible());
                QVERIFY(cancel->height() >= cancel->fontMetrics().height());
                QVERIFY(cancel->height() <= cancel->fontMetrics().height() + 6);
                QSignalSpy commands(&window, &MainWindow::commandRequested);
                cancel->click();
                QCOMPARE(commands.last()[0].toString(), QString("cancel"));
            }
        }
    }
    void readableDimmedContext() {
        LogView view;
        QFont font = view.font();
        font.setPointSizeF(14);
        font.setWeight(QFont::Bold);
        view.setTextFont(font);
        view.setLineNumbers(false);
        view.setMarkerVisibility(1);
        auto palette = view.palette();
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::Text, Qt::black);
        view.setPalette(palette);
        const auto snapshot = Snapshot::fromText("MMMMMMMM\nERROR");
        Filter filter;
        filter.pattern = "ERROR";
        view.setDocument(snapshot, {filter}, analyze(snapshot, {filter}, {}), {});
        view.resize(400, 120);
        view.show();
        const auto darkestContext = [&] {
            const auto image = view.viewport()->grab().toImage();
            const auto scale = image.devicePixelRatio();
            int darkest = 255;
            const int height = qRound((QFontMetrics(view.font()).height() + 4) * scale);
            for (int y = 0; y < std::min(height, image.height()); ++y)
                for (int x = qRound(8 * scale); x < std::min(qRound(220 * scale), image.width()); ++x)
                    darkest = std::min(darkest, image.pixelColor(x, y).red());
            return darkest;
        };
        QTRY_VERIFY_WITH_TIMEOUT(darkestContext() < 120, 10000);
        QVERIFY(darkestContext() >= 85);
    }
    void proportionalScrollbarKeepsExactSteps() {
        LogView view;
        Analysis synthetic;
        const LineId count = LineId(1) << 42;
        synthetic.accepted = LineSet::all(count);
        // Exercise the scroll model without showing or reading the synthetic rows.
        view.setDocument(Snapshot::fromText("placeholder"), {}, synthetic, {});
        view.setFilteredOnly(true);
        auto* bar = view.verticalScrollBar();
        QCOMPARE(bar->maximum(), std::numeric_limits<int>::max());
        bar->setValue(bar->maximum());
        const auto end = view.firstVisibleRank();
        QVERIFY(end > LineId(std::numeric_limits<int>::max()));
        bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
        QCOMPARE(view.firstVisibleRank(), end - 1);
        bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
        QCOMPARE(view.firstVisibleRank(), end - 2);
        bar->triggerAction(QAbstractSlider::SliderSingleStepAdd);
        QCOMPARE(view.firstVisibleRank(), end - 1);
        view.selectAll();
        QCOMPARE(view.selection().count(), count);
        QVERIFY(view.selection().residentBytes() < 1024);
    }
    void sourceOriginSurvivesRecomposition() {
        LogView view;
        view.resize(400, 100);
        auto snapshot = Snapshot::fromText(QString("line\n").repeated(100));
        Analysis all;
        all.accepted = LineSet::all(snapshot->lineCount());
        view.setDocument(snapshot, {}, all, {});
        view.setFilteredOnly(true);
        view.verticalScrollBar()->setValue(20);
        QCOMPARE(view.firstVisibleRank(), LineId(20));
        Analysis even;
        LineSetBuilder builder;
        for (LineId row = 0; row < snapshot->lineCount(); row += 2) builder.add(row);
        even.accepted = builder.finish();
        view.setDocument(snapshot, {}, even, {});
        QCOMPARE(view.firstVisibleRank(), LineId(10));
    }
    void topmostIncludingColorAndSelectionContrast() {
        LogView view;
        view.resize(600, 150);
        auto snapshot = Snapshot::fromText("x");
        Filter first, second;
        first.background = QColor(Qt::red);
        second.background = QColor(Qt::green);
        Analysis analysis;
        analysis.accepted = LineSet::all(1);
        analysis.matches = {LineSet::all(1), LineSet::all(1)};
        view.setDocument(snapshot, {first, second}, analysis, {});
        view.setMarkerVisibility(1);
        view.show();
        QCOMPARE(view.viewport()->grab().toImage().pixelColor(200, 3), QColor(Qt::red));
        first.excluding = true;
        view.setDocument(snapshot, {first, second}, analysis, {});
        QCOMPARE(view.viewport()->grab().toImage().pixelColor(200, 3), QColor(Qt::green));
        view.goToLine(0);
        QCOMPARE(view.viewport()->grab().toImage().pixelColor(200, 3), view.palette().color(QPalette::Highlight));
    }
    void boundedRenderingRetainsFullSourceAndAsyncDoubleClick() {
        LogView view;
        view.resize(600, 200);
        const QString text(20000, 'x');
        auto snapshot = Snapshot::fromText(text);
        Analysis analysis;
        analysis.accepted = LineSet::all(1);
        view.setDocument(snapshot, {}, analysis, {});
        view.setMaxCharacters(10);
        view.setMarkerVisibility(1);
        QSignalSpy added(&view, &LogView::addFilterRequested);
        QSignalSpy errors(&view, &LogView::errorOccurred);
        view.show();
        view.viewport()->grab();
        QTest::mouseDClick(view.viewport(), Qt::LeftButton, {}, QPoint(120, 10));
        QTRY_COMPARE_WITH_TIMEOUT(added.count(), 1, 5000);
        QCOMPARE(added.first()[0].toString(), text);
        QCOMPARE(snapshot->line(0), text);
        QCOMPARE(errors.count(), 0);
        view.setZoom(150);
        QCOMPARE(view.zoom(), 150);
        view.viewport()->grab();
    }
    void loadingPreviewIsNotSelectableAndFinalDocumentReplacesIt() {
        LogView view;
        view.resize(600, 200);
        view.setLoadingPreview(QString("preview\n").repeated(10000));
        QCOMPARE(view.viewport()->accessibleName(), LogView::tr("Loading preview (not final)"));
        QCOMPARE(view.viewport()->accessibleDescription(),
            LogView::tr("Loading preview of a bounded prefix. Not final. Selection and analysis are unavailable."));
        view.show();
        view.viewport()->grab();
        view.selectAll();
        QCOMPARE(view.selection().count(), LineId(0));
        QSignalSpy added(&view, &LogView::addFilterRequested);
        QTest::mouseDClick(view.viewport(), Qt::LeftButton, {}, QPoint(100, 30));
        QCOMPARE(added.count(), 0);
        const auto final = Snapshot::fromText("final");
        Analysis analysis;
        analysis.accepted = LineSet::all(final->lineCount());
        view.setDocument(final, {}, analysis, {});
        view.goToLine(0);
        QCOMPARE(view.selection().count(), LineId(1));
        QCOMPARE(view.viewport()->accessibleName(), LogView::tr("Log text viewport"));
        view.viewport()->grab();
        view.setLoadingPreview(QStringList{"line one", "line two"});
        view.selectAll();
        QCOMPARE(view.selection().count(), LineId(0));
        view.clearLoadingPreview();
        QCOMPARE(view.viewport()->accessibleName(), LogView::tr("Log text viewport"));
        view.clearLoadingPreview();
    }
    void editorValidation() {
        bool validated = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            const auto closeOnExit = qScopeGuard([dialog] {
                if (dialog && dialog->isVisible()) dialog->reject();
            });
            QVERIFY(dialog);
            auto* regex = dialog->findChild<QCheckBox*>("filterRegex");
            auto* pattern = dialog->findChild<QComboBox*>("filterPattern");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            regex->setChecked(true);
            pattern->setEditText("[");
            buttons->button(QDialogButtonBox::Ok)->click();
            validated = !dialog->findChild<QLabel*>("validationError")->text().isEmpty();
            pattern->setEditText("valid.*");
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        Filter initial;
        initial.extraAttributes.insert("plugin-data", "retained");
        const auto result = editFilter(nullptr, initial, {"history"});
        QVERIFY(validated);
        QVERIFY(result.has_value());
        QVERIFY(result->regularExpression);
        QCOMPARE(result->pattern, QString("valid.*"));
        QCOMPARE(result->extraAttributes.value("plugin-data"), QString("retained"));
    }
    void compactCheckboxMouseAndKeyboard() {
        MainWindow window;
        Filter a, b;
        a.id = "a"; b.id = "b";
        a.pattern = "first"; b.pattern = "second";
        window.filterModel()->setFilters({a, b});
        window.filterModel()->setCounts({LineSet::all(3), LineSet::all(4)}, 10);
        auto* table = window.filterView();
        window.show();
        QTest::qWait(10);
        QVERIFY(table->horizontalHeader()->isHidden());
        QVERIFY(table->verticalHeader()->isHidden());
        QVERIFY(!table->showGrid());
        QSignalSpy changes(window.filterModel(), &FilterModel::filtersChanged);
        QSignalSpy commands(&window, &MainWindow::commandRequested);
        const auto rect = table->visualRect(window.filterModel()->index(0, 0));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 10, rect.center().y()));
        QCOMPARE(changes.count(), 1);
        QVERIFY(!window.filterModel()->filters()[0].enabled);
        QCOMPARE(commands.count(), 0);
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 10, rect.center().y()));
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, {}, QPoint(rect.left() + 10, rect.center().y()));
        QCOMPARE(changes.count(), 1);
        QCOMPARE(commands.count(), 0);
        table->selectAll();
        QTest::keyClick(table, Qt::Key_Space);
        QVERIFY(window.filterModel()->filters()[0].enabled);
        QVERIFY(!window.filterModel()->filters()[1].enabled);
        QTest::keyClick(table, Qt::Key_Return);
        QCOMPARE(commands.last()[0].toString(), QString("edit-filter"));
        QTest::keyClick(table, Qt::Key_Delete);
        QCOMPARE(commands.last()[0].toString(), QString("remove-filter"));
        QTest::keyClick(table, Qt::Key_Up, commandModifier());
        QCOMPARE(commands.last()[0].toString(), QString("dock-top"));
    }
    void uncheckedFilterOutline_data() {
        QTest::addColumn<QColor>("background");
        QTest::addColumn<bool>("checked");
        QTest::addColumn<bool>("selected");
        for (const auto& name : {"darkgoldenrod", "darkblue", "white"})
            for (const bool checked : {false, true})
                for (const bool selected : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(name)
                        .arg(checked ? "checked" : "unchecked").arg(selected ? "selected" : "unselected")))
                        << QColor(name) << checked << selected;
    }
    void uncheckedFilterOutline() {
        QFETCH(QColor, background);
        QFETCH(bool, checked);
        QFETCH(bool, selected);
        MainWindow window;
        Filter filter;
        filter.pattern = "test";
        filter.enabled = checked;
        filter.background = background;
        window.filterModel()->setFilters({filter});
        auto* table = window.filterView();
        window.show();
        QCoreApplication::processEvents();
        QStyleOptionViewItem option;
        option.initFrom(table);
        option.palette.setColor(QPalette::Highlight, QColor("#0078d7"));
        if (selected) option.state |= QStyle::State_Selected;
        else option.state &= ~QStyle::State_Selected;
        option.widget = table;
        option.rect = QRect(0, 0, 300, table->verticalHeader()->defaultSectionSize());
        option.fontMetrics = QFontMetrics(table->font());
        QImage image(option.rect.size(), QImage::Format_ARGB32);
        image.fill(background);
        {
            QPainter painter(&image);
            table->itemDelegate()->paint(&painter, option, window.filterModel()->index(0, 0));
        }
        const int size = std::min({
            table->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, table),
            table->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, table),
            option.rect.height() - 4});
        const int top = (option.rect.height() - size) / 2;
        const auto outline = qGray(background.rgb()) < 128 ? QColor(Qt::white) : QColor(Qt::black);
        QCOMPARE(image.pixelColor(3 + size / 2, top), outline);
        QCOMPARE(image.pixelColor(3 + size / 2, top - 1), background);
        QCOMPARE(image.pixelColor(3 + size / 2, top + size), background);
        QCOMPARE(image.pixelColor(3 + size / 4, top + size / 4),
            checked ? QColor("#0078d7") : QColor(Qt::white));
        if (checked) {
            int tickPixels = 0;
            for (int y = top + 3; y < top + size - 3; ++y)
                for (int x = 6; x < 3 + size - 3; ++x)
                    tickPixels += qGray(image.pixelColor(x, y).rgb()) > 230;
            QVERIFY(tickPixels >= 3);
        }
    }
    void selectedLogRowHasSolidEdges() {
        LogView view;
        view.resize(400, 140);
        auto palette = view.palette();
        palette.setColor(QPalette::Highlight, QColor("#0078d7"));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        view.setPalette(palette);
        const auto snapshot = Snapshot::fromText("selected line\nsecond line");
        Analysis analysis;
        analysis.accepted = LineSet::all(2);
        view.setDocument(snapshot, {}, analysis, {});
        view.show();
        view.setFocus();
        view.goToLine(0);
        QTRY_VERIFY(view.hasFocus());
        const auto image = view.viewport()->grab().toImage();
        const int height = qRound((QFontMetrics(view.font()).height() + 4) * image.devicePixelRatio());
        QCOMPARE(image.pixelColor(image.width() - 2, 0), palette.color(QPalette::Highlight));
        QCOMPARE(image.pixelColor(image.width() - 2, height - 1), palette.color(QPalette::Highlight));
    }
    void selectedLineVisibleAfterUnhide_data() {
        QTest::addColumn<bool>("selectMatch");
        QTest::newRow("keep-original-selection") << false;
        QTest::newRow("keep-new-matching-selection") << true;
    }
    void selectedLineVisibleAfterUnhide() {
        QFETCH(bool, selectMatch);
        QStringList lines;
        for (int i = 0; i < 300; ++i) lines.append(QString::number(i));
        const auto snapshot = Snapshot::fromText(lines.join('\n'));
        LineSetBuilder matches;
        matches.add(0);
        matches.add(100);
        matches.add(200);
        Analysis analysis;
        analysis.accepted = matches.finish();
        LogView view;
        view.resize(400, 140);
        view.setDocument(snapshot, {}, analysis, {});
        view.goToLine(150);
        view.setFilteredOnly(true);
        if (selectMatch) view.goToLine(200);
        view.setFilteredOnly(false);
        const LineId expected = selectMatch ? 200 : 150;
        QCOMPARE(view.currentLine(), expected);
        QCOMPARE(view.selection().count(), LineId(1));
        QVERIFY(view.selection().contains(expected));
        const auto page = LineId(std::max(1,
            view.viewport()->height() / (QFontMetrics(view.font()).height() + 4)));
        QVERIFY(view.firstVisibleRank() <= expected);
        QVERIFY(expected - view.firstVisibleRank() < page);
    }
    void originalEditorLayoutAndColors_data() {
        QTest::addColumn<bool>("unequalHeights");
        QTest::newRow("native-control-heights") << false;
        QTest::newRow("different-control-heights") << true;
    }
    void originalEditorLayoutAndColors() {
        QFETCH(bool, unequalHeights);
        bool customAccepted = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            const auto closeOnExit = qScopeGuard([dialog] {
                if (dialog && dialog->isVisible()) dialog->reject();
            });
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("Add Filter"));
            auto* kind = dialog->findChild<QComboBox*>("filterKind");
            auto* foreground = dialog->findChild<QComboBox*>("filterForeground");
            auto* background = dialog->findChild<QComboBox*>("filterBackground");
            auto* pattern = dialog->findChild<QComboBox*>("filterPattern");
            QVERIFY(kind && foreground && background && pattern);
            QCOMPARE(kind->count(), 2);
            QCOMPARE(kind->itemText(0), QString("Matches text"));
            auto* grid = qobject_cast<QGridLayout*>(kind->parentWidget()->layout());
            QVERIFY(grid);
            if (unequalHeights) {
                kind->setFixedHeight(kind->height());
                foreground->setFixedHeight(kind->height() + 2);
                dialog->layout()->activate();
                grid->activate();
                QVERIFY(kind->mapTo(dialog, QPoint()).y() != foreground->mapTo(dialog, QPoint()).y());
            }
            const auto rowOf = [grid](QWidget* widget) {
                int row = -1, column = -1, rows = 0, columns = 0;
                grid->getItemPosition(grid->indexOf(widget), &row, &column, &rows, &columns);
                return row;
            };
            QCOMPARE(rowOf(kind), 0);
            QCOMPARE(rowOf(foreground), 0);
            QCOMPARE(rowOf(background), 0);
            const auto centerY = [dialog](QWidget* widget) {
                return widget->mapTo(dialog, QPoint(0, widget->height() / 2)).y();
            };
            QVERIFY(std::abs(centerY(kind) - centerY(foreground)) <= 1);
            QVERIFY(std::abs(centerY(foreground) - centerY(background)) <= 1);
            QVERIFY(pattern->mapTo(dialog, QPoint()).y() > kind->mapTo(dialog, QPoint()).y());
            QVERIFY(dialog->findChildren<QGroupBox*>().isEmpty());
            QVERIFY(!dialog->findChild<QPushButton*>("filterDetails"));
            auto* description = dialog->findChild<QLineEdit*>("filterDescription");
            auto* checks = dialog->findChild<QCheckBox*>("filterExcluding");
            QVERIFY(description && description->isVisible());
            QVERIFY(description->mapTo(dialog, QPoint()).y() > pattern->mapTo(dialog, QPoint()).y());
            QVERIFY(checks->mapTo(dialog, QPoint()).y() > description->mapTo(dialog, QPoint()).y());
            QStringList names = QColor::colorNames();
            names.removeAll("transparent");
            QSet<QRgb> expectedColors;
            for (const auto& name : names) expectedColors.insert(QColor(name).rgba());
            QCOMPARE(background->count(), expectedColors.size() + 2);
            QCOMPARE(foreground->count(), background->count());
            QSet<QRgb> actualColors;
            QString previous;
            for (int i = 1; i < background->count() - 1; ++i) {
                const auto color = background->itemData(i).value<QColor>();
                QVERIFY(!actualColors.contains(color.rgba()));
                actualColors.insert(color.rgba());
                QCOMPARE(foreground->itemData(i).value<QColor>(), color);
                QVERIFY(foreground->itemIcon(i).isNull() == false);
                const auto label = background->itemText(i).toLower();
                QVERIFY(previous < label);
                previous = label;
            }
            QCOMPARE(actualColors, expectedColors);
            QCOMPARE(background->findData(QColor("darkgrey")), background->findData(QColor("darkgray")));
            QCOMPARE(background->findData(QColor("cyan")), background->findData(QColor("aqua")));
            QStyleOptionViewItem option;
            option.initFrom(background);
            option.font = background->font();
            option.fontMetrics = QFontMetrics(option.font);
            option.rect = QRect(0, 0, 200, option.fontMetrics.height() + 4);
            const auto index = background->model()->index(background->findData(QColor("khaki")), 0);
            QCOMPARE(background->itemDelegate()->sizeHint(option, index).height(),
                option.fontMetrics.height() + 4);
            QImage swatch(option.rect.size(), QImage::Format_ARGB32);
            swatch.fill(Qt::white);
            {
                QPainter painter(&swatch);
                background->itemDelegate()->paint(&painter, option, index);
            }
            QCOMPARE(swatch.pixelColor(190, 2), QColor("khaki"));
            background->setCurrentIndex(background->findData(QColor("khaki")) - 1);
            QTest::keyClick(background, Qt::Key_Down);
            QCOMPARE(pattern->palette().color(QPalette::Base), QColor("khaki"));
            foreground->setCurrentIndex(foreground->findData(QColor("red")));
            QCOMPARE(pattern->palette().color(QPalette::Text), QColor("red"));
            const auto textColor = dialog->palette().color(QPalette::ButtonText);
            const auto buttonColor = dialog->palette().color(QPalette::Button);
            QCOMPARE(foreground->palette().color(QPalette::ButtonText), textColor);
            foreground->setCurrentIndex(foreground->findData(QColor("yellow")));
            QCOMPARE(foreground->palette().color(QPalette::ButtonText), textColor);
            QCOMPARE(foreground->palette().color(QPalette::Button), buttonColor);
            QTimer::singleShot(0, [&] {
                auto* picker = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
                const auto closeOnExit = qScopeGuard([picker] {
                    if (picker && picker->isVisible()) picker->reject();
                });
                QVERIFY(picker);
                picker->setCurrentColor(QColor("#123456"));
                picker->accept();
                customAccepted = true;
            });
            foreground->setCurrentIndex(foreground->count() - 2);
            QTest::keyClick(foreground, Qt::Key_Down);
            QCOMPARE(foreground->currentData().value<QColor>(), QColor("#123456"));
            QTimer::singleShot(0, [] {
                auto* picker = qobject_cast<QColorDialog*>(QApplication::activeModalWidget());
                const auto closeOnExit = qScopeGuard([picker] {
                    if (picker && picker->isVisible()) picker->reject();
                });
                QVERIFY(picker);
                picker->setCurrentColor(QColor("#abcdef"));
                picker->reject();
            });
            foreground->setCurrentIndex(foreground->count() - 1);
            QCOMPARE(foreground->currentData().value<QColor>(), QColor("#123456"));
            background->setCurrentIndex(0);
            kind->setCurrentIndex(1);
            QVERIFY(pattern->isHidden());
            QVERIFY(!dialog->findChild<QCheckBox*>("filterRegex")->isEnabled());
            dialog->findChild<QComboBox*>("filterMarker")->setCurrentIndex(7);
            dialog->findChild<QCheckBox*>("filterExcluding")->setChecked(true);
            description->setText("marker detail");
            const auto screenshot = qEnvironmentVariable("TEXTSCOPE_DIALOG_SCREENSHOT");
            if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        Filter initial;
        initial.enabled = false;
        const auto result = editFilter(nullptr, initial, {"history"}, false, true);
        QVERIFY(customAccepted && result);
        QCOMPARE(result->foreground, std::optional<QColor>(QColor("#123456")));
        QVERIFY(!result->background);
        QVERIFY(!result->enabled);
        QVERIFY(result->excluding);
        QCOMPARE(result->kind, Filter::Kind::Marker);
        QCOMPARE(result->marker, 7);
        QCOMPARE(result->description, QString("marker detail"));
    }
    void modalCallbackEarlyReturnClosesDialog() {
        QDialog dialog;
        bool ran = false;
        QTimer::singleShot(0, &dialog, [&] {
            const auto closeOnExit = qScopeGuard([&dialog] { dialog.reject(); });
            ran = true;
            return;
        });
        QCOMPARE(dialog.exec(), int(QDialog::Rejected));
        QVERIFY(ran);
    }
    void editorCancelPreservesFilter() {
        Filter initial;
        initial.pattern = "original";
        initial.description = "description";
        initial.foreground = QColor("red");
        initial.background = QColor("khaki");
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            const auto closeOnExit = qScopeGuard([dialog] {
                if (dialog && dialog->isVisible()) dialog->reject();
            });
            QVERIFY(dialog);
            dialog->findChild<QComboBox*>("filterPattern")->setEditText("changed");
            dialog->findChild<QLineEdit*>("filterDescription")->setText("changed");
            dialog->findChild<QComboBox*>("filterBackground")->setCurrentIndex(0);
            dialog->reject();
        });
        QVERIFY(!editFilter(nullptr, initial, {}));
        QCOMPARE(initial.pattern, QString("original"));
        QCOMPARE(initial.description, QString("description"));
        QCOMPARE(initial.background, std::optional<QColor>(QColor("khaki")));
    }
    void colorPickerThemes_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void colorPickerThemes() {
        QFETCH(bool, dark);
        const auto original = QApplication::palette();
        auto palette = original;
        palette.setColor(QPalette::Window, dark ? QColor("#202020") : QColor(Qt::white));
        palette.setColor(QPalette::Base, dark ? QColor("#202020") : QColor(Qt::white));
        palette.setColor(QPalette::Button, dark ? QColor("#353535") : QColor("#eeeeee"));
        palette.setColor(QPalette::Text, dark ? Qt::white : Qt::black);
        palette.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
        QApplication::setPalette(palette);
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            const auto closeOnExit = qScopeGuard([dialog] {
                if (dialog && dialog->isVisible()) dialog->reject();
            });
            QVERIFY(dialog);
            auto* foreground = dialog->findChild<QComboBox*>("filterForeground");
            auto* background = dialog->findChild<QComboBox*>("filterBackground");
            for (const auto& color : {QColor(Qt::yellow), QColor(Qt::black), QColor(Qt::white)}) {
                foreground->setCurrentIndex(foreground->findData(color));
                QCOMPARE(foreground->palette().color(QPalette::ButtonText),
                    dialog->palette().color(QPalette::ButtonText));
                QCOMPARE(foreground->palette().color(QPalette::Button),
                    dialog->palette().color(QPalette::Button));
                QVERIFY(!foreground->itemIcon(foreground->currentIndex()).isNull());
                QStyleOptionViewItem option;
                option.initFrom(foreground);
                option.fontMetrics = QFontMetrics(foreground->font());
                option.rect = QRect(0, 0, 220, option.fontMetrics.height() + 4);
                QImage image(option.rect.size(), QImage::Format_ARGB32);
                image.fill(dialog->palette().color(QPalette::Base));
                {
                    QPainter painter(&image);
                    foreground->itemDelegate()->paint(&painter, option,
                        foreground->model()->index(foreground->currentIndex(), 0));
                }
                QCOMPARE(image.pixelColor(210, 2), dialog->palette().color(QPalette::Base));
            }
            background->setCurrentIndex(background->findData(QColor("khaki")));
            QCOMPARE(background->palette().color(QPalette::Button), QColor("khaki"));
            dialog->reject();
        });
        const auto result = editFilter(nullptr, {}, {});
        QApplication::setPalette(original);
        QVERIFY(!result);
    }
    void cleanupTestCase() {
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
    }
};
QTEST_MAIN(WidgetsTest)
#include "widgets_test.moc"
