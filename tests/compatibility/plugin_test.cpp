#include "sources/plugins.hpp"
#include "persistence/persistence.hpp"
#include <QTemporaryDir>
#include <QtTest>

using namespace textscope;

class PluginTest : public QObject {
    Q_OBJECT
private slots:
    void nativeStreamingParser()
    {
#ifdef TEXTSCOPE_TEST_PLUGIN_DIRECTORY
        Plugins plugins;
        plugins.discover(QString::fromUtf8(TEXTSCOPE_TEST_PLUGIN_DIRECTORY));
        QVERIFY2(plugins.errors().isEmpty(), qPrintable(plugins.errors().join('\n')));
        QCOMPARE(plugins.descriptions().size(), 1);
        QTemporaryDir directory;
        const auto path = directory.filePath("sample.textscopedemo");
        writeFileAtomically(path, "#TEXTSCOPE-DEMO\nfirst\nsecond\n");
        const auto snapshot = plugins.open(path, {}, {}, {});
        QCOMPARE(snapshot->lineCount(), LineId(2));
        QCOMPARE(snapshot->line(0), QString("first"));
        QCOMPARE(snapshot->line(1), QString("second"));
        const auto flag = std::make_shared<std::atomic_bool>(true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, plugins.open(path, {}, flag, {}));
#else
        QSKIP("Build with TEXTSCOPE_BUILD_EXAMPLE_PLUGIN=ON to test native plug-in loading.");
#endif
    }
};

QTEST_MAIN(PluginTest)
#include "plugin_test.moc"
