#include "app/session.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QTemporaryDir>
#include <QPointer>
#include <QProcess>
#include <QThreadPool>
#include <QMimeData>
#include <QKeySequence>
#include <QFileDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QDir>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QtTest>

using namespace textscope;

class AppTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDirectory_;
private slots:
    void initTestCase()
    {
        if (!qEnvironmentVariableIsSet("TEXTSCOPE_TEST_NATIVE_DIALOG"))
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setOrganizationName("TextScopeTests");
        QCoreApplication::setApplicationName("Integration");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
        QApplication::setWindowIcon(QIcon(":/icons/textscope.png"));
    }
    void applicationIcon()
    {
        const QImage image(":/icons/textscope.png");
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(512, 512));
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(308, 124), QColor("#45d6e8"));
        QCOMPARE(image.pixelColor(308, 234), QColor("#ffbf47"));
        Session session;
        QVERIFY(!QApplication::windowIcon().isNull());
        QVERIFY(!session.window()->windowIcon().isNull());
        for (const int size : {16, 32, 64, 256}) {
            const auto pixmap = session.window()->windowIcon().pixmap(size, size);
            QVERIFY(!pixmap.isNull());
            QCOMPARE(pixmap.size(), QSize(size, size));
        }
#ifdef Q_OS_MACOS
        const auto executableDirectory = QFileInfo(QString::fromUtf8(TEXTSCOPE_TEST_EXECUTABLE)).absolutePath();
        const QDir contents(executableDirectory + "/..");
        QFile icon(contents.filePath("Resources/textscope.icns"));
        QVERIFY(icon.open(QIODevice::ReadOnly));
        QCOMPARE(icon.read(4), QByteArray("icns"));
        QFile plist(contents.filePath("Info.plist"));
        QVERIFY(plist.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&plist);
        bool foundIcon = false;
        int lowercaseNames = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != u"key") continue;
            const auto key = xml.readElementText();
            if (key == "CFBundleExecutable" || key == "CFBundleName" || key == "CFBundleDisplayName") {
                QVERIFY(xml.readNextStartElement());
                QCOMPARE(xml.readElementText(), QString("textscope"));
                ++lowercaseNames;
                continue;
            }
            if (key != "CFBundleIconFile") continue;
            QVERIFY(xml.readNextStartElement());
            QCOMPARE(xml.readElementText(), QString("textscope.icns"));
            foundIcon = true;
        }
        QVERIFY(!xml.hasError());
        QVERIFY(foundIcon);
        QCOMPARE(lowercaseNames, 3);
#endif
    }
    void documentWorkflow()
    {
        QTemporaryDir directory;
        const auto input = directory.filePath("sample.log");
        writeFileAtomically(input, "INFO ready\nERROR failed\nINFO done\n");
        FilterFile filters;
        Filter error;
        error.id = "error";
        error.pattern = "ERROR";
        error.foreground = QColor(Qt::red);
        filters.filters.push_back(error);
        const auto filterPath = directory.filePath("sample.tat");
        saveFilters(filterPath, filters);
        Startup startup;
        startup.input = input;
        startup.filters.append(filterPath);
        Session session(startup);
        QCOMPARE(session.window()->accessibleName(), QString("TextScope"));
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY(session.window()->windowTitle().endsWith("TextScope"));
        QCOMPARE(session.document()->lineCount(), LineId(3));
        QCOMPARE(session.analysis().accepted.count(), LineId(1));
        const auto screenshot = qEnvironmentVariable("TEXTSCOPE_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(session.window()->grab().save(screenshot));
        }
        auto* filtered = session.window()->findChild<QAction*>("filtered-only");
        QVERIFY(filtered);
        filtered->trigger();
        QVERIFY(session.window()->logView()->filteredOnly());
        session.window()->logView()->selectAll();
        QCOMPARE(session.window()->logView()->selection().count(), LineId(1));
        session.window()->findChild<QAction*>("copy")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(QApplication::clipboard()->text(), QString("ERROR failed"));
        QVERIFY(QApplication::clipboard()->mimeData()->html().contains("#ff0000"));
        QVERIFY(QFile::remove(input));
        QCOMPARE(session.document()->line(1), QString("ERROR failed"));
        session.window()->logView()->toggleMarkerRequested(0);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY(session.markers()[0].contains(1));
        session.window()->logView()->toggleMarkerRequested(0);
        session.window()->logView()->toggleMarkerRequested(0);
        session.window()->logView()->toggleMarkerRequested(1);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY(session.markers()[0].contains(1));
        QVERIFY(session.markers()[1].contains(1));
    }
    void documentationPreview()
    {
        Session session;
        session.pasteText(
            "2026-11-01 09:14:00 INFO  gateway    Starting service on port 8080\n"
            "2026-11-01 09:14:00 INFO  config     Loaded production configuration\n"
            "2026-11-01 09:14:01 INFO  database   Connection pool ready (16 connections)\n"
            "2026-11-01 09:14:01 INFO  cache      Connected to cache cluster\n"
            "2026-11-01 09:14:02 INFO  gateway    Health checks passing\n"
            "2026-11-01 09:14:04 INFO  api        GET /api/orders request_id=8f2a\n"
            "2026-11-01 09:14:04 WARN  database   Slow query: orders_by_customer (842 ms)\n"
            "2026-11-01 09:14:05 INFO  api        Response 200 in 871 ms request_id=8f2a\n"
            "2026-11-01 09:14:06 INFO  worker     Processing export job=1042\n"
            "2026-11-01 09:14:07 ERROR upstream   Payment provider timed out after 3000 ms\n"
            "2026-11-01 09:14:07 INFO  retry      Scheduled payment retry attempt=1\n"
            "2026-11-01 09:14:08 INFO  api        GET /health response=200 duration=2ms\n"
            "2026-11-01 09:14:09 WARN  cache      Hit rate below threshold: 68%\n"
            "2026-11-01 09:14:10 INFO  worker     Export progress: 12500 / 20000 rows\n"
            "2026-11-01 09:14:11 INFO  upstream   Payment retry succeeded\n"
            "2026-11-01 09:14:12 INFO  api        POST /api/orders request_id=9c31\n"
            "2026-11-01 09:14:12 ERROR validation Missing required field: shipping_address\n"
            "2026-11-01 09:14:12 INFO  api        Response 422 in 4 ms request_id=9c31\n"
            "2026-11-01 09:14:13 INFO  worker     Export job=1042 completed (20000 rows)\n"
            "2026-11-01 09:14:14 INFO  metrics    Active requests=3 queue_depth=0\n"
            "2026-11-01 09:14:15 INFO  gateway    Ready for incoming requests\n", false);
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr && !session.busy(), 10000);
        Filter errors;
        errors.id = "errors";
        errors.pattern = "ERROR";
        errors.description = "Failures requiring investigation";
        errors.foreground = QColor("#9c2020");
        errors.background = QColor("#ffe5e5");
        Filter warnings;
        warnings.id = "warnings";
        warnings.pattern = "WARN";
        warnings.description = "Slow queries and service warnings";
        warnings.foreground = QColor("#805500");
        warnings.background = QColor("#fff2cc");
        Filter request;
        request.id = "request";
        request.pattern = "request_id=8f2a";
        request.description = "Trace one request through the log";
        request.foreground = QColor("#135a9c");
        request.background = QColor("#e4f0ff");
        FilterFile filters;
        filters.filters = {errors, warnings, request};
        auto* data = new QMimeData;
        data->setData("application/x-textscope-filters", writeFilters(filters));
        QApplication::clipboard()->setMimeData(data);
        session.window()->findChild<QAction*>("paste-filters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.document()->lineCount(), LineId(21));
        QCOMPARE(session.analysis().accepted.count(), LineId(6));
        session.window()->resize(1160, 760);
        session.window()->logView()->setFocus();
        const auto renderedFilterColors = [&] {
            const auto image = session.window()->logView()->viewport()->grab().toImage();
            bool red = false, amber = false, blue = false;
            for (int y = 0; y < image.height(); ++y) {
                const auto color = image.pixelColor(image.width() - 24, y);
                red = red || color == errors.background.value();
                amber = amber || color == warnings.background.value();
                blue = blue || color == request.background.value();
            }
            return red && amber && blue;
        };
        QTRY_VERIFY_WITH_TIMEOUT(renderedFilterColors(), 10000);
        const auto screenshot = qEnvironmentVariable("TEXTSCOPE_DOCUMENTATION_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QVERIFY(session.window()->grab().save(screenshot));
        }
    }
    void commandLineBranding()
    {
        QCOMPARE(QFileInfo(QString::fromUtf8(TEXTSCOPE_TEST_EXECUTABLE)).completeBaseName(),
            QString("textscope"));
        QProcess process;
        process.start(QString::fromUtf8(TEXTSCOPE_TEST_EXECUTABLE), {"--version"});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(process.readAllStandardOutput().replace("\r\n", "\n"),
            QByteArray("TextScope 0.1.0\n"));
        process.start(QString::fromUtf8(TEXTSCOPE_TEST_EXECUTABLE), {"--help"});
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().startsWith("Usage: textscope "));
    }
    void filterClipboardCompatibility_data()
    {
        QTest::addColumn<QString>("mime");
        QTest::newRow("textscope") << QString("application/x-textscope-filters");
        QTest::newRow("legacy") << QString("application/x-tat-filters");
        QTest::newRow("original") << QString("TEXTANALYSISTOOL.NET_FILTERS");
    }
    void filterClipboardCompatibility()
    {
        QFETCH(QString, mime);
        Session session;
        FilterFile filters;
        Filter filter;
        filter.pattern = "ERROR";
        filters.filters.push_back(filter);
        auto* data = new QMimeData;
        data->setData(mime, writeFilters(filters));
        QApplication::clipboard()->setMimeData(data);
        session.window()->findChild<QAction*>("paste-filters")->trigger();
        QCOMPARE(session.window()->filterModel()->filters().size(), std::size_t(1));
        QCOMPARE(session.window()->filterModel()->filters()[0].pattern, QString("ERROR"));
        session.window()->filterView()->selectRow(0);
        session.window()->findChild<QAction*>("copy-filters")->trigger();
        const auto* copied = QApplication::clipboard()->mimeData();
        QVERIFY(copied->hasFormat("application/x-textscope-filters"));
        QVERIFY(copied->hasFormat("application/x-tat-filters"));
        QCOMPARE(readFilters(copied->data("application/x-textscope-filters")).filters[0].pattern,
            QString("ERROR"));
    }
    void newerLoadWins()
    {
        QTemporaryDir directory;
        const auto first = directory.filePath("first.log");
        const auto second = directory.filePath("second.log");
        writeFileAtomically(first, QByteArray(1024 * 1024, 'x') + '\n');
        writeFileAtomically(second, "new document\n");
        Session session;
        session.open(first);
        session.open(second);
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.document()->line(0), QString("new document"));
    }
    void emptyAndClipboard()
    {
        Session session;
        session.pasteText("", false);
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.document()->lineCount(), LineId(0));
        session.pasteText("one\ntwo", false);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.document()->lineCount(), LineId(2));
    }
    void closeWhileLoading()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("closing.log");
        writeFileAtomically(path, QByteArray(8 * 1024 * 1024, 'x'));
        auto* session = new Session;
        QPointer<Session> guard(session);
        session->open(path);
        session->window()->close();
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 10000);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
    }
    void zoomSynchronization()
    {
        Session session;
        session.window()->logView()->setZoom(125);
        QCOMPARE(session.window()->logView()->zoom(), 125);
        session.window()->findChild<QAction*>("zoom-in")->trigger();
        QCOMPARE(session.window()->logView()->zoom(), 135);
        session.window()->findChild<QAction*>("zoom-reset")->trigger();
        QCOMPARE(session.window()->logView()->zoom(), 100);
    }
    void platformCommandModifier()
    {
        QCOMPARE(Qt::KeyboardModifiers(commandModifier()),
            QKeySequence(QKeySequence::Copy)[0].keyboardModifiers());
    }
    void openMenuLoadsSelectedFile() {
        QTemporaryDir directory;
        const auto path = directory.filePath("file chosen from menu.log");
        writeFileAtomically(path, "opened from File menu\nsecond line\n");
        Session session;
        session.window()->findChild<QAction*>("open")->trigger();
        auto* dialog = session.window()->findChild<QFileDialog*>("openFileDialog");
        QVERIFY(dialog);
        QCOMPARE(dialog->fileMode(), QFileDialog::ExistingFile);
        QVERIFY(dialog->isVisible());
        dialog->selectFile(path);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr && !session.busy(), 10000);
        QCOMPARE(session.document()->line(0), QString("opened from File menu"));
        QCOMPARE(session.document()->lineCount(), LineId(2));
        QTest::qWait(10);
        session.window()->findChild<QAction*>("open")->trigger();
        dialog = session.window()->findChild<QFileDialog*>("openFileDialog");
        QVERIFY(dialog);
        dialog->reject();
        QCOMPARE(session.document()->line(0), QString("opened from File menu"));
    }
    void addShortcutAndCheckboxRecompute() {
        Session session;
        session.pasteText("first\nsecond", false);
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr && !session.busy(), 10000);
        session.window()->activateWindow();
        session.window()->logView()->setFocus();
        QTest::qWait(10);
        QTimer::singleShot(0, [] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->windowTitle(), QString("Add Filter"));
            dialog->findChild<QComboBox*>("filterPattern")->setEditText("first");
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        QTest::keyClick(session.window()->logView(), Qt::Key_N, commandModifier());
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QCOMPARE(session.window()->filterModel()->rowCount(), 1);
        QCOMPARE(session.analysis().accepted.count(), LineId(1));
        const auto first = session.window()->filterModel()->index(0, 0);
        auto* table = session.window()->filterView();
        const auto rect = table->visualRect(first);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, QPoint(10, rect.center().y()));
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY(!session.window()->filterModel()->filters()[0].enabled);
        QCOMPARE(session.analysis().accepted.count(), LineId(2));
        QCOMPARE(session.analysis().scannedBytes, std::uint64_t(0));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, QPoint(10, rect.center().y()));
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 10000);
        QVERIFY(session.window()->filterModel()->filters()[0].enabled);
        QCOMPARE(session.analysis().accepted.count(), LineId(1));
        QCOMPARE(session.analysis().scannedBytes, std::uint64_t(0));
    }
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
