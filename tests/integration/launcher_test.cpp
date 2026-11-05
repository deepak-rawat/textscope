#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_MACOS
#include <unistd.h>
#include <vector>
#endif

namespace {
#ifdef Q_OS_WIN
const QString suffix = ".exe";
#else
const QString suffix;
#endif
}

class LauncherTest : public QObject {
    Q_OBJECT
private slots:
    void helpAndVersion() {
        for (const auto& argument : {"--help", "-h", "--version", "-v"}) {
            QProcess process;
            process.start(QString::fromUtf8(TEXTSCOPE_TEST_LAUNCHER), {argument});
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 0);
            const auto output = process.readAllStandardOutput().replace("\r\n", "\n");
            if (QByteArray(argument).contains('h'))
                QVERIFY(output.startsWith("Usage: tscope "));
            else
                QCOMPARE(output, QByteArray("TextScope 0.1.0\n"));
            QVERIFY(process.readAllStandardError().isEmpty());
        }
    }
    void launchErrors_data() {
        QTest::addColumn<bool>("invalid");
        QTest::newRow("missing") << false;
        QTest::newRow("invalid") << true;
    }
    void launchErrors() {
        QFETCH(bool, invalid);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto launcher = temporary.filePath("tscope" + suffix);
        QVERIFY(QFile::copy(QString::fromUtf8(TEXTSCOPE_TEST_LAUNCHER), launcher));
        if (invalid) {
            QFile application(temporary.filePath("textscope" + suffix));
            QVERIFY(application.open(QIODevice::WriteOnly));
            application.close();
            QVERIFY(application.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        QProcess process;
        process.start(launcher, {"relative.log"});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains(invalid ? "cannot start" : "missing or not executable"));
    }
    void forwarding_data() {
        QTest::addColumn<QString>("layout");
        QTest::newRow("adjacent") << "";
#ifdef Q_OS_MACOS
        QTest::newRow("build-bundle") << "textscope.app/Contents/MacOS";
        QTest::newRow("installed-bundle") << "";
#endif
    }
    void forwarding() {
        QFETCH(QString, layout);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto directory = temporary.filePath("working space");
        QVERIFY(QDir().mkpath(directory));
        const auto launcherDirectory = QString(QTest::currentDataTag()) == "installed-bundle"
            ? temporary.filePath("textscope.app/Contents/MacOS") : temporary.filePath("bin");
        QVERIFY(QDir().mkpath(launcherDirectory));
        auto launcher = QDir(launcherDirectory).filePath("tscope" + suffix);
        QVERIFY(QFile::copy(QString::fromUtf8(TEXTSCOPE_TEST_LAUNCHER), launcher));
#ifdef Q_OS_MACOS
        if (QString(QTest::currentDataTag()) == "installed-bundle") {
            const auto link = temporary.filePath("tscope");
            QVERIFY(QFile::link(launcher, link));
            launcher = link;
        }
#endif
        const auto appDirectory = QDir(launcherDirectory).filePath(layout);
        QVERIFY(QDir().mkpath(appDirectory));
        QVERIFY(QFile::copy(QString::fromUtf8(TEXTSCOPE_TEST_LAUNCHER_FIXTURE),
            QDir(appDirectory).filePath("textscope" + suffix)));
        const auto output = temporary.filePath("result.json");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("TEXTSCOPE_LAUNCHER_OUTPUT", output);
        const QStringList arguments{
            "relative log.txt", "--filters", "filters file.tat", "--config", "settings.xml",
            "--line", "12", "/Filters:legacy filter.tat", QString::fromUtf8("caf\xc3\xa9"),
            QString::fromUtf8("cafe\xcc\x81"), "quote\"inside", "trailing\\", ""
        };
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(directory);
#ifdef Q_OS_MACOS
        // Feed original UTF-8 bytes to test normalization-free forwarding.
        std::vector<QByteArray> encoded{QFile::encodeName(launcher)};
        for (const auto& argument : arguments)
            encoded.push_back(argument.toUtf8());
        std::vector<char*> raw;
        for (auto& argument : encoded)
            raw.push_back(argument.data());
        raw.push_back(nullptr);
        std::vector<QByteArray> encodedEnvironment;
        for (const auto& entry : environment.toStringList())
            encodedEnvironment.push_back(entry.toUtf8());
        std::vector<char*> rawEnvironment;
        for (auto& entry : encodedEnvironment)
            rawEnvironment.push_back(entry.data());
        rawEnvironment.push_back(nullptr);
        process.setChildProcessModifier([&] {
            execve(encoded.front().constData(), raw.data(), rawEnvironment.data());
            _exit(127);
        });
#endif
        process.start(launcher, arguments);
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().isEmpty());
        QVERIFY(process.readAllStandardError().isEmpty());
        QVERIFY(!QFile::exists(output + ".done"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(output + ".done"), 10000);
        QFile result(output);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(result.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        const auto object = document.object();
        QCOMPARE(QDir(object.value("cwd").toString()).canonicalPath(), QDir(directory).canonicalPath());
        QCOMPARE(object.value("arguments").toArray(), QJsonArray::fromStringList(arguments));
    }
};

QTEST_GUILESS_MAIN(LauncherTest)
#include "launcher_test.moc"
