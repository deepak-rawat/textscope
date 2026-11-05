#include "core/core.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <cstdio>
#include <limits>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
std::uint64_t processPeakRss() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS usage{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &usage, sizeof(usage)))
        throw std::runtime_error(QString("Cannot read process memory (Windows error %1)")
            .arg(GetLastError()).toStdString());
    return std::uint64_t(usage.PeakWorkingSetSize);
#else
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("Cannot read process memory");
#ifdef Q_OS_MACOS
    return std::uint64_t(usage.ru_maxrss);
#else
    return std::uint64_t(usage.ru_maxrss) * 1024;
#endif
#endif
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("TextScope immutable snapshot and scan benchmark"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("generate-mib"), QStringLiteral("Generate an owned input of this many MiB (default 64)."), QStringLiteral("size")});
    parser.addOption({QStringLiteral("file"), QStringLiteral("Read an existing input file without modifying it."), QStringLiteral("path")});
    parser.addOption({QStringLiteral("budget-mib"), QStringLiteral("Snapshot RAM budget in MiB (default 32)."), QStringLiteral("size"), QStringLiteral("32")});
    parser.addOption({QStringLiteral("literal-case-sensitive"), QStringLiteral("Use case-sensitive ASCII matching for the isolated literal scan only.")});
    parser.addOption({QStringLiteral("direct"), QStringLiteral("Benchmark explicitly direct buffered access instead of an immutable capture.")});
    parser.process(application);
    try {
        if (parser.isSet(QStringLiteral("file")) && parser.isSet(QStringLiteral("generate-mib")))
            throw std::runtime_error("Choose either --file or --generate-mib");
        bool valid = false;
        const auto budgetMiB = parser.value(QStringLiteral("budget-mib")).toULongLong(&valid);
        if (!valid || budgetMiB > std::numeric_limits<std::size_t>::max() / (1024 * 1024))
            throw std::runtime_error("Invalid memory budget");
        QTemporaryFile generated(QStringLiteral(".textscope-benchmark-XXXXXX"));
        QString path = parser.value(QStringLiteral("file"));
        if (path.isEmpty()) {
            const auto requested = parser.isSet(QStringLiteral("generate-mib"))
                ? parser.value(QStringLiteral("generate-mib")) : QStringLiteral("64");
            const auto mib = requested.toULongLong(&valid);
            if (!valid || mib > qulonglong(std::numeric_limits<qint64>::max()) / (1024 * 1024))
                throw std::runtime_error("Invalid generated size");
            if (!generated.open())
                throw std::runtime_error("Cannot create benchmark input in the working directory");
            const QByteArray row =
                "2026-01-01 INFO user=123 request completed\r\n"
                "2026-01-01 ERROR user=42 request failed\n"
                "2026-01-01 WARNING user=7 retry scheduled\r";
            const auto chunk = row.repeated(8192);
            qulonglong remaining = mib * 1024 * 1024;
            while (remaining) {
                const auto amount = qint64(std::min<qulonglong>(remaining, qulonglong(chunk.size())));
                if (generated.write(chunk.constData(), amount) != amount)
                    throw std::runtime_error("Cannot write generated input");
                remaining -= qulonglong(amount);
            }
            if (!generated.flush())
                throw std::runtime_error("Cannot flush generated input");
            path = generated.fileName();
        }
        textscope::OpenOptions options;
        options.memoryBudget = std::size_t(budgetMiB * 1024 * 1024);
        options.direct = parser.isSet(QStringLiteral("direct"));
        QElapsedTimer clock;
        clock.start();
        const auto snapshot = textscope::Snapshot::open(path, options);
        const auto captureNs = clock.nsecsElapsed();
        std::vector<textscope::Filter> filters(3);
        filters[0].pattern = QStringLiteral("ERROR");
        filters[1].pattern = QStringLiteral("WARNING");
        filters[2].pattern = QStringLiteral("\\buser=7\\b");
        filters[2].regularExpression = true;
        filters[2].excluding = true;
        qint64 literalNs = 0;
        textscope::LineId literalAccepted = 0;
        std::uint64_t literalScannedBytes = 0;
        auto literalFilter = filters[0];
        literalFilter.caseSensitive = parser.isSet(QStringLiteral("literal-case-sensitive"));
        {
            clock.restart();
            const auto literal = textscope::analyze(snapshot, {literalFilter}, {});
            literalNs = clock.nsecsElapsed();
            literalAccepted = literal.accepted.count();
            literalScannedBytes = literal.scannedBytes;
        }
        clock.restart();
        const auto scan = textscope::analyze(snapshot, filters, {});
        const auto scanNs = clock.nsecsElapsed();
        std::vector<std::optional<textscope::LineSet>> cached;
        std::size_t matchBytes = 0;
        for (const auto& match : scan.matches) {
            cached.emplace_back(match);
            matchBytes += match.residentBytes();
        }
        filters[0].foreground = QColor(Qt::red);
        filters[1].enabled = false;
        clock.restart();
        const auto composed = textscope::analyze(snapshot, filters, {}, cached);
        const auto cacheNs = clock.nsecsElapsed();
        const auto peakRss = processPeakRss();
        QJsonObject output{
            {QStringLiteral("captureIndexMs"), double(captureNs) / 1e6},
            {QStringLiteral("scanMs"), double(scanNs) / 1e6},
            {QStringLiteral("singleLiteralMs"), double(literalNs) / 1e6},
            {QStringLiteral("singleLiteralCaseSensitive"), literalFilter.caseSensitive},
            {QStringLiteral("singleLiteralAccepted"), double(literalAccepted)},
            {QStringLiteral("singleLiteralScannedBytes"), double(literalScannedBytes)},
            {QStringLiteral("cacheComposeMs"), double(cacheNs) / 1e6},
            {QStringLiteral("bytes"), double(snapshot->byteCount())},
            {QStringLiteral("direct"), snapshot->isDirect()},
            {QStringLiteral("lines"), double(snapshot->lineCount())},
            {QStringLiteral("accepted"), double(scan.accepted.count())},
            {QStringLiteral("cachedAccepted"), double(composed.accepted.count())},
            {QStringLiteral("scannedBytes"), double(scan.scannedBytes)},
            {QStringLiteral("cachedScannedBytes"), double(composed.scannedBytes)},
            {QStringLiteral("snapshotResidentBytes"), double(snapshot->residentBytes())},
            {QStringLiteral("lineIndexBytes"), double(snapshot->indexBytes())},
            {QStringLiteral("matchResidentBytes"), double(matchBytes)},
            {QStringLiteral("acceptedResidentBytes"), double(scan.accepted.residentBytes())},
            {QStringLiteral("processPeakRssBytes"), double(peakRss)}
        };
        const auto json = QJsonDocument(output).toJson(QJsonDocument::Compact);
        std::puts(json.constData());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
