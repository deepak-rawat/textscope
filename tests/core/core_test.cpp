#include "core/core.hpp"
#include "core/predicate.hpp"
#include <QtTest>
#include <QtCore5Compat/QTextCodec>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <limits>
#include <random>
#include <stdexcept>

using namespace textscope;

class CoreTest : public QObject {
    Q_OBJECT
private:
    static Filter text(const QString& pattern, bool excluding = false) {
        Filter result;
        result.pattern = pattern;
        result.excluding = excluding;
        return result;
    }
    static std::shared_ptr<const Snapshot> bytes(const QByteArray& data,
        const OpenOptions& options = {}) {
        QTemporaryFile file(QStringLiteral(".textscope-test-XXXXXX"));
        if (!file.open() || file.write(data) != data.size() || !file.flush())
            throw std::runtime_error("Cannot create test input");
        return Snapshot::open(file.fileName(), options);
    }
private slots:
    void boundaries() {
        auto snapshot = bytes("one\r\ntwo\rthree\n\nlast");
        QCOMPARE(snapshot->lineCount(), LineId(5));
        QCOMPARE(snapshot->line(0), QStringLiteral("one"));
        QCOMPARE(snapshot->line(1), QStringLiteral("two"));
        QCOMPARE(snapshot->line(2), QStringLiteral("three"));
        QCOMPARE(snapshot->line(3), QString());
        QCOMPARE(snapshot->line(4), QStringLiteral("last"));
        QCOMPARE(bytes("")->lineCount(), LineId(0));
        QCOMPARE(bytes("\n")->lineCount(), LineId(1));
        QCOMPARE(bytes("a\n")->lineCount(), LineId(1));
        QCOMPARE(bytes("\r\n\r\n")->lineCount(), LineId(2));
        QByteArray large(256 * 1024 - 1, 'a');
        large += "\r\n";
        large += QString::fromUtf8("α😀").toUtf8();
        large += '\r';
        OpenOptions options;
        options.memoryBudget = 1024;
        auto boundary = bytes(large, options);
        QCOMPARE(boundary->lineCount(), LineId(2));
        QCOMPARE(boundary->line(0, 17), QString(17, QLatin1Char('a')));
        QCOMPARE(boundary->line(1), QString::fromUtf8("α😀"));
        std::vector<QString> streamed;
        boundary->forEachLine([&](LineId, const QString& line) { streamed.push_back(line); });
        QCOMPARE(streamed.size(), std::size_t(2));
        QCOMPARE(streamed[0].size(), qsizetype(256 * 1024 - 1));
        QCOMPARE(streamed[1], boundary->line(1));
        auto query = text(QStringLiteral("a$"));
        query.regularExpression = true;
        QCOMPARE(analyze(boundary, {query}, {}).matches[0].count(), LineId(1));
    }
    void codecs() {
        const auto content = QString::fromUtf8("Älpha\r\n日本\r😀\nlast");
        for (const QByteArray codecName : {QByteArray("UTF-8"), QByteArray("UTF-16LE"),
             QByteArray("UTF-16BE"), QByteArray("UTF-32LE"), QByteArray("UTF-32BE")}) {
            auto* codec = QTextCodec::codecForName(codecName);
            QVERIFY(codec);
            auto data = codec->fromUnicode(content);
            OpenOptions options;
            options.encoding = "windows-1252"; // BOM must override the supplied codec.
            if (codecName == "UTF-8")
                data.prepend(QByteArray::fromHex("efbbbf"));
            auto snapshot = bytes(data, options);
            QCOMPARE(snapshot->lineCount(), LineId(4));
            QCOMPARE(snapshot->line(0), QString::fromUtf8("Älpha"));
            QCOMPARE(snapshot->line(1), QString::fromUtf8("日本"));
            QCOMPARE(snapshot->line(2), QString::fromUtf8("😀"));
            QCOMPARE(analyze(snapshot, {text(QString::fromUtf8("日本"))}, {}).accepted.count(), LineId(1));
        }
        OpenOptions legacy;
        legacy.encoding = "windows-1252";
        auto snapshot = bytes(QByteArray::fromHex("636166e90d0a80"), legacy);
        QCOMPARE(snapshot->line(0), QString::fromUtf8("café"));
        QCOMPARE(snapshot->line(1), QString::fromUtf8("€"));
        legacy.encoding = "Shift-JIS";
        auto* codec = QTextCodec::codecForName(legacy.encoding);
        auto sjis = bytes(codec->fromUnicode(QString::fromUtf8("日本\n東京")), legacy);
        QCOMPARE(sjis->line(1), QString::fromUtf8("東京"));
        legacy.encoding = "ISO-2022-JP";
        const auto statefulData = QByteArray::fromHex("1b2442467c4b5c1b28420a1b2442456c357e1b2842");
        auto stateful = bytes(statefulData, legacy);
        QCOMPARE(stateful->line(0), QString::fromUtf8("日本"));
        QCOMPARE(stateful->line(1), QString::fromUtf8("東京"));
        QCOMPARE(analyze(stateful, {text(QString::fromUtf8("東京"))}, {}).accepted.count(), LineId(1));
        legacy.direct = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bytes(statefulData, legacy));
        legacy.direct = false;
        legacy.encoding = "not-an-encoding";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bytes("abc", legacy));
    }
    void malformedAndHugeLimits() {
        auto malformed = bytes(QByteArray::fromHex("61c3"));
        QCOMPARE(malformed->line(0), QStringLiteral("a") + QChar::ReplacementCharacter);
        QCOMPARE(analyze(malformed, {text(QString(QChar::ReplacementCharacter))}, {}).accepted.count(), LineId(1));
        auto tail = bytes(QByteArray::fromHex("c3"));
        QCOMPARE(tail->lineCount(), LineId(1));
        QCOMPARE(analyze(tail, {text(QString(QChar::ReplacementCharacter))}, {}).accepted.count(), LineId(1));
        QCOMPARE(malformed->line(0, std::numeric_limits<qsizetype>::max()), malformed->line(0));
        OpenOptions options;
        options.encoding = "UTF-16LE";
        auto odd = bytes(QByteArray::fromHex("610001"), options);
        QCOMPARE(odd->line(0), QStringLiteral("a") + QChar::ReplacementCharacter);
        QCOMPARE(analyze(odd, {text(QString(QChar::ReplacementCharacter))}, {}).accepted.count(), LineId(1));
    }
    void unicodeAndCache() {
        auto snapshot = Snapshot::fromText(QString::fromUtf8("CAFÉ\ncafé\nΣσς\nStraße\nascii ERROR\n"));
        std::vector<Filter> filters{text(QString::fromUtf8("café")), text(QString::fromUtf8("σ")),
                                    text(QStringLiteral("error"))};
        auto result = analyze(snapshot, filters, {});
        QCOMPARE(result.matches[0].count(), LineId(2));
        QCOMPARE(result.matches[1].count(), LineId(1));
        QCOMPARE(result.matches[2].at(0), LineId(4));
        QCOMPARE(result.scannedBytes, snapshot->byteCount());
        auto changed = filters[0];
        changed.id = QStringLiteral("other");
        changed.description = QStringLiteral("description");
        changed.enabled = false;
        changed.excluding = true;
        changed.background = QColor(Qt::red);
        changed.extraAttributes.insert(QStringLiteral("unknown"), QStringLiteral("value"));
        QCOMPARE(predicateKey(changed), predicateKey(filters[0]));
        changed.caseSensitive = true;
        QVERIFY(predicateKey(changed) != predicateKey(filters[0]));
        std::vector<std::optional<LineSet>> cached;
        for (auto set : result.matches)
            cached.emplace_back(set);
        filters[1].excluding = true;
        auto hit = analyze(snapshot, filters, {}, cached);
        QCOMPARE(hit.scannedBytes, std::uint64_t(0));
        QCOMPARE(hit.accepted.count(), LineId(3));
        cached[2].reset();
        QCOMPARE(analyze(snapshot, filters, {}, cached).scannedBytes, snapshot->byteCount());
    }
    void literalBytePath() {
        const auto content = QString(256 * 1024 - 1, QLatin1Char('a')) +
            QString::fromUtf8("\r\nASCII ERROR\rerrOR ö\nſS\nlast ERROR");
        auto snapshot = Snapshot::fromText(content);
        QVERIFY(snapshot->hasUtf8Storage());
        std::vector<Filter> literals{text(QStringLiteral("error")), text(QStringLiteral("s")),
                                     text(QStringLiteral("last"))};
        auto fast = analyze(snapshot, literals, {});
        Filter forceUnicode;
        forceUnicode.regularExpression = true;
        forceUnicode.pattern = QStringLiteral("(?!)");
        forceUnicode.enabled = false;
        auto mixed = literals;
        mixed.push_back(forceUnicode);
        auto decoded = analyze(snapshot, mixed, {});
        for (std::size_t i = 0; i < literals.size(); ++i) {
            QCOMPARE(fast.matches[i].count(), decoded.matches[i].count());
            for (LineId rank = 0; rank < fast.matches[i].count(); ++rank)
                QCOMPARE(fast.matches[i].at(rank), decoded.matches[i].at(rank));
        }
        QCOMPARE(fast.matches[0].count(), LineId(3));
        literals[0].caseSensitive = true;
        literals[0].pattern = QStringLiteral("ERROR");
        QCOMPARE(analyze(snapshot, {literals[0]}, {}).matches[0].count(), LineId(2));
        std::vector<QByteArray> streamed;
        snapshot->forEachUtf8Line([&](LineId, QByteArrayView line) {
            streamed.emplace_back(line.data(), line.size());
        });
        QCOMPARE(streamed.size(), std::size_t(snapshot->lineCount()));
        for (std::size_t i = 0; i < streamed.size(); ++i)
            QCOMPARE(QString::fromUtf8(streamed[i]), snapshot->line(i));
    }
    void immutableAndDirect() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("source.log")));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write("original\nsecond"), qint64(15));
        QVERIFY(file.flush());
        file.close();
        OpenOptions disk;
        disk.memoryBudget = 1;
        auto snapshot = Snapshot::open(file.fileName(), disk);
        disk.direct = true;
        auto direct = Snapshot::open(file.fileName(), disk);
        QVERIFY(direct->isDirect());
        QCOMPARE(direct->line(0), QStringLiteral("original"));
        const auto path = file.fileName();
#ifdef Q_OS_WIN
        // Qt's direct read handle shares reads/writes, but not deletion.
        QVERIFY(!QFile::remove(path));
        direct.reset();
#endif
        QVERIFY(QFile::remove(path));
        QCOMPARE(snapshot->line(1), QStringLiteral("second"));
        QCOMPARE(analyze(snapshot, {text(QStringLiteral("original"))}, {}).accepted.count(), LineId(1));
#ifndef Q_OS_WIN
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, direct->verifyUnchanged());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, direct->line(0));
#endif
        QFile changing(directory.filePath(QStringLiteral("changing.log")));
        QVERIFY(changing.open(QIODevice::WriteOnly));
        QCOMPARE(changing.write("one"), qint64(3));
        QVERIFY(changing.flush());
        changing.close();
        auto source = Snapshot::open(changing.fileName(), disk);
        const auto modified = QFileInfo(changing.fileName()).lastModified().addSecs(4);
        QVERIFY(modified.isValid());
        QFile writer(changing.fileName());
        QVERIFY(writer.open(QIODevice::WriteOnly));
        QCOMPARE(writer.write("two"), qint64(3));
        QVERIFY(writer.flush());
        QVERIFY(writer.setFileTime(modified, QFileDevice::FileModificationTime));
        writer.close();
        QCOMPARE(QFileInfo(changing.fileName()).lastModified(), modified);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, source->verifyUnchanged());
    }
    void directUnicodeSourceChanges_data() {
        QTest::addColumn<bool>("sameSize");
        QTest::newRow("same-size-with-new-timestamp") << true;
        QTest::newRow("different-size") << false;
    }
    void directUnicodeSourceChanges() {
        QFETCH(bool, sameSize);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("source-\u03bb.log"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("one\n"), qint64(4));
        file.close();
        OpenOptions options;
        options.direct = true;
        const auto snapshot = Snapshot::open(path, options);
        QCOMPARE(snapshot->line(0), QString("one"));
        const auto modified = QFileInfo(path).lastModified().addSecs(4);
        QVERIFY(modified.isValid());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray replacement = sameSize ? QByteArray("two\n") : QByteArray("longer\n");
        QCOMPARE(file.write(replacement), qint64(replacement.size()));
        QVERIFY(file.flush());
        if (sameSize)
            QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        file.close();
        if (sameSize)
            QCOMPARE(QFileInfo(path).lastModified(), modified);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshot->verifyUnchanged());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshot->line(0));
    }
    void spillAndRank() {
        LineSetBuilder builder(64);
        for (LineId i = 0; i < 100000; i += 2)
            builder.add(i);
        const auto set = builder.finish();
        QCOMPARE(set.count(), LineId(50000));
        QCOMPARE(set.at(12345), LineId(24690));
        QCOMPARE(set.rankBefore(24691), LineId(12346));
        QVERIFY(set.contains(99998));
        QVERIFY(!set.contains(99999));
        QVERIFY(set.residentBytes() < 8192);
        QCOMPARE(*set.next(99998, true), LineId(0));
        QVERIFY(!set.next(99998, true, false));
        QCOMPARE(*set.next(0, false), LineId(99998));
        const LineId max = std::numeric_limits<LineId>::max();
        auto all = LineSet::all(max);
        QCOMPARE(all.count(), max);
        QCOMPARE(all.at(max - 1), max - 1);
        QCOMPARE(all.rankBefore(max), max);
        QVERIFY(!all.contains(max));
        QCOMPARE(*all.next(max, false), max - 1);
        QCOMPARE(*all.next(max - 1, true), LineId(0));
        LineSetBuilder high(0);
        high.addRange(max - 3, max);
        auto highest = high.finish();
        QCOMPARE(highest.count(), LineId(4));
        QCOMPARE(highest.at(3), max);
        QCOMPARE(highest.rankBefore(max), LineId(3));
        QCOMPARE(*highest.next(max, true), max - 3);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, highest.at(4));
        LineSetBuilder overflow;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, overflow.addRange(0, max));
        LineSetBuilder monotonic;
        monotonic.add(4);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, monotonic.add(3));
        QByteArray many;
        for (int i = 0; i < 20000; ++i)
            many += "a\n";
        OpenOptions options;
        options.memoryBudget = 128;
        auto snapshot = bytes(many, options);
        QCOMPARE(snapshot->lineCount(), LineId(20000));
        QCOMPARE(snapshot->line(19999), QStringLiteral("a"));
        QCOMPARE(snapshot->indexBytes(), std::uint64_t(320000));
        QVERIFY(snapshot->residentBytes() < 16384);
    }
    void bulkRangeBlocks() {
        std::vector<std::pair<LineId, LineId>> input;
        for (LineId i = 0; i < 20000; ++i)
            input.emplace_back(i * 3, i * 3 + 1);
        LineSetBuilder builder(64);
        builder.addRanges(input.data(), input.size());
        auto set = builder.finish();
        QCOMPARE(set.count(), LineId(40000));
        QCOMPARE(set.rangeCount(), std::uint64_t(20000));
        QCOMPARE(set.at(39999), LineId(59998));
        std::array<std::pair<LineId, LineId>, 127> block;
        std::uint64_t offset = 0;
        while (const auto count = set.readRanges(offset, block.data(), block.size())) {
            for (std::size_t i = 0; i < count; ++i)
                QVERIFY(block[i] == input[std::size_t(offset) + i]);
            offset += count;
        }
        QCOMPARE(offset, set.rangeCount());
        QVERIFY(set.residentBytes() < 8192);
        QCOMPARE(set.readRanges(0, nullptr, 0), std::size_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, set.readRanges(0, nullptr, 1));
        LineSetBuilder coalescing(0);
        const std::pair<LineId, LineId> ranges[]{{1, 2}, {3, 5}, {4, 7}, {10, 12}};
        coalescing.addRanges(ranges, 4);
        coalescing.addRange(13, 15);
        auto merged = coalescing.finish();
        QCOMPARE(merged.rangeCount(), std::uint64_t(2));
        QCOMPARE(merged.count(), LineId(13));
        LineSetBuilder errors;
        const std::pair<LineId, LineId> unsorted[]{{1, 2}, {8, 9}, {3, 4}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, errors.addRanges(unsorted, 3));
        auto partial = errors.finish();
        QCOMPARE(partial.count(), LineId(4));
        QCOMPARE(partial.at(3), LineId(9));
    }
    void filterLogicAndMarkers() {
        auto snapshot = Snapshot::fromText(QStringLiteral("apple\nbanana\nboth apple banana\nnone\n"));
        auto apple = text(QStringLiteral("apple"));
        auto banana = text(QStringLiteral("banana"));
        QCOMPARE(analyze(snapshot, {}, {}).accepted.count(), LineId(4));
        QCOMPARE(analyze(snapshot, {apple, banana}, {}).accepted.count(), LineId(3));
        banana.excluding = true;
        auto result = analyze(snapshot, {apple, banana}, {});
        QCOMPARE(result.accepted.count(), LineId(1));
        QCOMPARE(result.accepted.at(0), LineId(0));
        QCOMPARE(analyze(snapshot, {banana}, {}).accepted.count(), LineId(2));
        apple.enabled = false;
        QCOMPARE(analyze(snapshot, {apple, banana}, {}).accepted.count(), LineId(2));
        Markers markers;
        LineSetBuilder marker;
        marker.add(1);
        marker.add(3);
        marker.add(100);
        markers[7] = marker.finish();
        Filter marked;
        marked.kind = Filter::Kind::Marker;
        marked.marker = 7;
        auto selected = analyze(snapshot, {marked}, markers);
        QCOMPARE(selected.scannedBytes, std::uint64_t(0));
        QCOMPARE(selected.accepted.count(), LineId(2));
        QCOMPARE(selected.accepted.at(1), LineId(3));
        marked.marker = 8;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateFilter(marked));
        auto universe = LineSet::all(std::numeric_limits<LineId>::max());
        apple.enabled = true;
        QCOMPARE(compose(universe.count(), {apple}, {universe}).count(), universe.count());
    }
    void randomizedComposition() {
        std::mt19937 random(42);
        for (int trial = 0; trial < 50; ++trial) {
            std::vector<Filter> filters(5);
            std::vector<LineSet> sets;
            bool includes = false;
            for (auto& filter : filters) {
                filter.enabled = random() % 3 != 0;
                filter.excluding = random() % 2 != 0;
                includes |= filter.enabled && !filter.excluding;
                LineSetBuilder builder(32);
                for (LineId line = 0; line < 200; ++line)
                    if (random() % 3 == 0)
                        builder.add(line);
                sets.push_back(builder.finish());
            }
            auto result = compose(200, filters, sets);
            LineId expectedCount = 0;
            for (LineId line = 0; line < 200; ++line) {
                bool include = !includes, exclude = false;
                for (std::size_t i = 0; i < filters.size(); ++i) {
                    if (!filters[i].enabled || !sets[i].contains(line))
                        continue;
                    if (filters[i].excluding) exclude = true;
                    else include = true;
                }
                QCOMPARE(result.contains(line), include && !exclude);
                expectedCount += include && !exclude;
            }
            QCOMPARE(result.count(), expectedCount);
        }
    }
    void regexErrorsAndCancellation() {
        auto filter = text(QStringLiteral("["));
        filter.regularExpression = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateFilter(filter));
        auto snapshot = Snapshot::fromText(QString::fromUtf8("日本 123\nABC\nabc\n"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, analyze(snapshot, {filter}, {}));
        filter.pattern = QStringLiteral("\\p{Han}+\\s+\\d+");
        QCOMPARE(analyze(snapshot, {filter}, {}).accepted.count(), LineId(1));
        filter.pattern = QStringLiteral("^abc$");
        QCOMPARE(analyze(snapshot, {filter}, {}).accepted.count(), LineId(2));
        filter.caseSensitive = true;
        QCOMPARE(analyze(snapshot, {filter}, {}).accepted.count(), LineId(1));
        filter.pattern = QStringLiteral("(*NO_AUTO_POSSESS)(*NO_START_OPT)(a+)+$");
        auto pathological = Snapshot::fromText(QString(20000, QLatin1Char('a')) + QLatin1Char('!'));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, analyze(pathological, {filter}, {}));
        auto cancel = std::make_shared<std::atomic_bool>(true);
        try {
            analyze(snapshot, {filter}, {}, {}, cancel);
            QFAIL("Expected cancellation");
        } catch (const std::runtime_error& error) {
            QCOMPARE(QByteArray(error.what()), QByteArray("Cancelled"));
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Snapshot::fromText(QStringLiteral("text"), cancel));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, compose(10, {}, {}, cancel));
        cancel->store(false);
        auto large = bytes(QByteArray(512 * 1024, 'x'));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, analyze(large, {text(QStringLiteral("missing"))}, {}, {}, cancel,
            [&](std::uint64_t, std::uint64_t) { cancel->store(true); }));
    }
    void regexCalloutCancellation_data() {
        QTest::addColumn<bool>("jit");
        QTest::newRow("interpreter") << false;
        QTest::newRow("jit") << true;
    }
    void regexCalloutCancellation() {
        QFETCH(bool, jit);
        CancelFlag cancel = std::make_shared<std::atomic_bool>(false);
        QCOMPARE(detail::cancellationCallout(nullptr, &cancel), 0);
        const auto pattern = QStringLiteral("a+");
        int error = 0;
        PCRE2_SIZE offset = 0;
        const std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> code(
            pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.utf16()),
                PCRE2_SIZE(pattern.size()), PCRE2_AUTO_CALLOUT, &error, &offset, nullptr),
            &pcre2_code_free);
        QVERIFY(code);
        if (jit) {
            std::uint32_t supported = 0;
            QCOMPARE(pcre2_config(PCRE2_CONFIG_JIT, &supported), 0);
            if (!supported)
                QSKIP("PCRE2 was built without JIT support");
            QCOMPARE(pcre2_jit_compile(code.get(), PCRE2_JIT_COMPLETE), 0);
        }
        const std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> context(
            pcre2_match_context_create(nullptr), &pcre2_match_context_free);
        const std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data(
            pcre2_match_data_create_from_pattern(code.get(), nullptr), &pcre2_match_data_free);
        QVERIFY(context);
        QVERIFY(data);
        QCOMPARE(pcre2_set_callout(context.get(), [](pcre2_callout_block* block, void* state) {
            static_cast<CancelFlag*>(state)->get()->store(true);
            return detail::cancellationCallout(block, state);
        }, &cancel), 0);
        const auto subject = QStringLiteral("aaa");
        QCOMPARE(pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(subject.utf16()),
            PCRE2_SIZE(subject.size()), 0, 0, data.get(), context.get()), PCRE2_ERROR_CALLOUT);
        QVERIFY(cancel->load());
        cancel->store(false);
        auto filter = text(pattern);
        filter.regularExpression = true;
        detail::Predicate predicate(filter, cancel);
        QVERIFY(predicate.matches(subject, true));
        cancel->store(true);
        try {
            predicate.matches(subject, true);
            QFAIL("Expected cancellation");
        } catch (const std::runtime_error& exception) {
            QCOMPARE(QByteArray(exception.what()), QByteArray("Cancelled"));
        }
    }
    void navigation() {
        auto snapshot = Snapshot::fromText(QStringLiteral("x\nn\nx\nn\n"));
        auto query = text(QStringLiteral("x"));
        QCOMPARE(*find(snapshot, query, 0, true, false), LineId(2));
        QVERIFY(!find(snapshot, query, 2, true, false));
        QCOMPARE(*find(snapshot, query, 2, true, true), LineId(0));
        QCOMPARE(*find(snapshot, query, 2, false, false), LineId(0));
        QVERIFY(!find(snapshot, query, 0, false, false));
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "core_test.moc"
