#include "app/session.hpp"
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace textscope;

class PerformanceTest : public QObject {
    Q_OBJECT
private slots:
    void diskBackedViewportAndCachedToggles()
    {
        QTemporaryDir directory;
        QCoreApplication::setOrganizationName("TextScopeTests");
        QCoreApplication::setApplicationName("Performance");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        const auto inputPath = directory.filePath("performance.log");
        QFile input(inputPath);
        QVERIFY(input.open(QIODevice::WriteOnly));
        QByteArray block;
        const QByteArray line = "INFO transaction completed with stable source content\n"
            "ERROR04 transaction failed with stable source content\n";
        while (block.size() < 1024 * 1024) block += line;
        for (int i = 0; i < 64; ++i) QCOMPARE(input.write(block), qint64(block.size()));
        input.close();
        FilterFile filters;
        for (int i = 0; i < 10; ++i) {
            Filter filter;
            filter.id = QString::number(i);
            filter.pattern = QString("ERROR%1").arg(i, 2, 10, QChar('0'));
            filter.caseSensitive = true;
            filters.filters.push_back(filter);
        }
        const auto filterPath = directory.filePath("performance.tat");
        saveFilters(filterPath, filters);
        Startup startup;
        startup.input = inputPath;
        startup.filters.append(filterPath);
        QElapsedTimer total;
        total.start();
        Session session(startup);
        QSignalSpy preview(&session, &Session::previewAvailable);
        QTRY_VERIFY_WITH_TIMEOUT(preview.count() > 0 || session.document() != nullptr, 5000);
        session.window()->logView()->viewport()->repaint();
        const auto firstViewportMs = total.elapsed();
        QTRY_VERIFY_WITH_TIMEOUT(session.document() != nullptr, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto loadAndAnalyzeMs = total.elapsed();
        QVERIFY(session.document()->byteCount() >= 64 * 1024 * 1024);
        QVERIFY(session.analysis().accepted.count() > 0);
        auto* view = session.window()->logView();
        view->goToLine(session.document()->lineCount() / 2);
        QTest::qWait(100);
        std::vector<double> frames;
        for (int i = 0; i < 100; ++i) {
            QElapsedTimer frame;
            frame.start();
            view->viewport()->repaint();
            frames.push_back(frame.nsecsElapsed() / 1e6);
        }
        std::sort(frames.begin(), frames.end());
        QElapsedTimer toggle;
        toggle.start();
        auto* disable = session.window()->findChild<QAction*>("disable-all");
        QVERIFY(disable);
        disable->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        const auto disableMs = toggle.elapsed();
        QCOMPARE(session.analysis().scannedBytes, std::uint64_t(0));
        QCOMPARE(session.analysis().accepted.count(), session.document()->lineCount());
        toggle.restart();
        session.window()->findChild<QAction*>("enable-all")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!session.busy(), 30000);
        QCOMPARE(session.analysis().scannedBytes, std::uint64_t(0));
        QVERIFY(session.analysis().accepted.count() < session.document()->lineCount());
        const QJsonObject metrics{
            {"bytes", static_cast<qint64>(session.document()->byteCount())},
            {"lines", static_cast<qint64>(session.document()->lineCount())},
            {"filters", 10},
            {"loadAndAnalyzeMs", loadAndAnalyzeMs},
            {"firstVerifiedPrefixPaintMs", firstViewportMs},
            {"warmRepaintP95Ms", frames[95]},
            {"warmRepaintP99Ms", frames[99]},
            {"disableCachedMs", disableMs},
            {"enableCachedMs", toggle.elapsed()},
            {"toggleScannedBytes", 0}
        };
        qInfo().noquote() << QJsonDocument(metrics).toJson(QJsonDocument::Compact);
    }
};

QTEST_MAIN(PerformanceTest)
#include "performance_test.moc"
