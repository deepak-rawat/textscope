#include "persistence/persistence.hpp"
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QtTest>

using namespace textscope;

class PersistenceTest : public QObject {
    Q_OBJECT
private slots:
    void defaultAndSavedFonts() {
        QTemporaryDir directory;
        QSettings settings(directory.filePath("fonts.ini"), QSettings::IniFormat);
        const auto defaults = loadPreferences(settings);
#ifdef Q_OS_WIN
        QCOMPARE(defaults.font.pointSizeF(), 10.0);
        QCOMPARE(defaults.font.weight(), QFont::Medium);
        if (QFontDatabase::families().contains("Consolas"))
            QCOMPARE(defaults.font.family(), QString("Consolas"));
#else
        QCOMPARE(defaults.font, QFontDatabase::systemFont(QFontDatabase::FixedFont));
#endif
        QFont saved("monospace", 13, QFont::Light);
        settings.setValue("font", saved);
        QCOMPARE(loadPreferences(settings).font, saved);
    }
    void legacyAttributes()
    {
        const auto file = readFilters(R"xml(
<TextAnalysisTool.NET version="2016-01-08" showOnlyFilteredLines="True" custom="retained">
<filters>
<filter enabled="y" excluding="n" description="errors" foreColor="ff0000"
 type="matches_text" case_sensitive="n" regex="n" text="ERROR &amp; warning" future="keep"/>
<filter enabled="n" excluding="y" type="marked" marker="8" backColor="abcdef"/>
</filters></TextAnalysisTool.NET>)xml");
        QCOMPARE(file.filters.size(), std::size_t(2));
        QVERIFY(file.filteredOnly);
        QCOMPARE(file.filters[0].pattern, QString("ERROR & warning"));
        QCOMPARE(file.filters[0].foreground, std::optional<QColor>(QColor("#ff0000")));
        QCOMPARE(file.filters[1].marker, 7);
        const auto restored = readFilters(writeFilters(file));
        QCOMPARE(restored.attributes["custom"], QString("retained"));
        QCOMPARE(restored.filters[0].extraAttributes["future"], QString("keep"));
        QCOMPARE(restored.filters[1].background, std::optional<QColor>(QColor("#abcdef")));
    }
    void rejectsInvalid()
    {
        QVERIFY_EXCEPTION_THROWN(readFilters("<foo/>"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readFilters("<TextAnalysisTool.NET><filters><filter type=\"unknown\"/></filters></TextAnalysisTool.NET>"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readFilters("<TextAnalysisTool.NET><filters><filter type=\"marked\" marker=\"9\"/></filters></TextAnalysisTool.NET>"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readFilters("<TextAnalysisTool.NET><filters><filter type=\"matches_text\" enabled=\"maybe\"/></filters></TextAnalysisTool.NET>"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readFilters("<TextAnalysisTool.NET><filters><filter type=\"matches_text\" regex=\"y\" text=\"[\"/></filters></TextAnalysisTool.NET>"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readFilters("<TextAnalysisTool.NET><filters>"), std::runtime_error);
    }
    void savesTransactionally()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FilterFile file;
        Filter filter;
        filter.pattern = "test";
        file.filters.push_back(filter);
        const auto path = directory.filePath("filters.tat");
        saveFilters(path, file);
        QCOMPARE(loadFilters(path).filters[0].pattern, QString("test"));
        QVERIFY_EXCEPTION_THROWN(saveFilters(directory.filePath("missing/filters.tat"), file), std::runtime_error);
        QCOMPARE(loadFilters(path).filters[0].pattern, QString("test"));
    }
    void preferencesRoundTrip()
    {
        Preferences p;
        p.font = QFont("monospace", 13);
        p.encoding = "GB18030";
        p.tabWidth = 8;
        p.zoom = 150;
        p.copyLineNumbers = true;
        p.selectionColor = QColor("#abcdef");
        const auto bytes = writePreferences(p);
        QVERIFY(bytes.contains("<TextScope version=\"1\">"));
        QVERIFY(!bytes.contains("TextAnalysisTool.Qt"));
        const auto q = readPreferences(bytes, {});
        QCOMPARE(q.font, p.font);
        QCOMPARE(q.encoding, p.encoding);
        QCOMPARE(q.tabWidth, p.tabWidth);
        QCOMPARE(q.zoom, p.zoom);
        QCOMPARE(q.selectionColor, p.selectionColor);
        QVERIFY(q.copyLineNumbers);
        auto legacyBytes = bytes;
        legacyBytes.replace("TextScope", "TextAnalysisTool.Qt");
        const auto legacy = readPreferences(legacyBytes, {});
        QCOMPARE(legacy.font, p.font);
        QCOMPARE(legacy.encoding, p.encoding);
        QCOMPARE(legacy.zoom, p.zoom);
        QCOMPARE(legacy.tabWidth, p.tabWidth);
        QVERIFY_EXCEPTION_THROWN(readPreferences("<TextAnalysisTool.NET/>", {}), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readPreferences("<TextScope><option name=\"TabSize\" value=\"0\"/></TextScope>", {}), std::runtime_error);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            readPreferences("<TextScope><option name=\"DefaultEncodingName\" value=\"not-a-codec\"/></TextScope>", {}));
    }
    void migratesLegacySettings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("textscope.ini");
        QSettings settings(path, QSettings::IniFormat);
        QSettings legacy(directory.filePath("legacy.ini"), QSettings::IniFormat);
        legacy.setValue("zoom", 150);
        legacy.setValue("patternHistory", QStringList{"ERROR", "WARNING"});
        legacy.setValue("layout/geometry", QByteArray("saved geometry"));
        legacy.sync();
        migrateLegacySettings(settings, legacy);
        QSettings restored(path, QSettings::IniFormat);
        QCOMPARE(restored.value("zoom").toInt(), 150);
        QCOMPARE(restored.value("patternHistory").toStringList(), QStringList({"ERROR", "WARNING"}));
        QCOMPARE(restored.value("layout/geometry").toByteArray(), QByteArray("saved geometry"));
        settings.setValue("zoom", 125);
        legacy.setValue("newLegacyKey", true);
        migrateLegacySettings(settings, legacy);
        QCOMPARE(settings.value("zoom").toInt(), 125);
        QVERIFY(!settings.contains("newLegacyKey"));
        QCOMPARE(legacy.value("zoom").toInt(), 150);
    }
    void rejectsInvalidLegacySettings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("legacy.ini");
        writeFileAtomically(path, "[broken\n");
        QSettings settings(directory.filePath("textscope.ini"), QSettings::IniFormat);
        QSettings legacy(path, QSettings::IniFormat);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, migrateLegacySettings(settings, legacy));
    }
};

QTEST_MAIN(PersistenceTest)
#include "persistence_test.moc"
