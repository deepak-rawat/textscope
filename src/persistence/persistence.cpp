#include "persistence.hpp"

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSaveFile>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QTextCodec>
#include <stdexcept>

namespace textscope {
namespace {
[[noreturn]] void fail(const QString& message)
{
    throw std::runtime_error(message.toStdString());
}

bool boolean(const QString& value)
{
    const auto v = value.toLower();
    if (v == "y" || v == "true" || v == "1")
        return true;
    if (v == "n" || v == "false" || v == "0")
        return false;
    fail("Invalid boolean: " + value);
}

int integer(const QString& value, int minimum, int maximum)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || result < minimum || result > maximum)
        fail("Value is outside its supported range: " + value);
    return result;
}

QColor color(QString value)
{
    if (!value.startsWith('#') && (value.size() == 6 || value.size() == 8))
        value.prepend('#');
    const QColor result(value);
    if (!result.isValid())
        fail("Invalid color: " + value);
    return result;
}

void checkXml(QXmlStreamReader& xml)
{
    if (xml.hasError())
        fail(QString("XML line %1: %2").arg(xml.lineNumber()).arg(xml.errorString()));
}

QString yesNo(bool value) { return value ? "y" : "n"; }
}

FilterFile readFilters(const QByteArray& bytes)
{
    if (bytes.size() > 16 * 1024 * 1024)
        fail("Filter files must be smaller than 16 MiB.");
    QXmlStreamReader xml(bytes);
    FilterFile result;
    if (!xml.readNextStartElement() || xml.name() != u"TextAnalysisTool.NET")
        fail("Not a TextAnalysisTool.NET filter file.");
    for (const auto& attr : xml.attributes())
        result.attributes.insert(attr.name().toString(), attr.value().toString());
    if (result.attributes.contains("showOnlyFilteredLines"))
        result.filteredOnly = boolean(result.attributes.value("showOnlyFilteredLines"));
    bool foundFilters = false;
    while (xml.readNextStartElement()) {
        if (xml.name() != u"filters")
            fail("Unrecognized filter-file element: " + xml.name().toString());
        if (foundFilters)
            fail("Duplicate filters element.");
        foundFilters = true;
        while (xml.readNextStartElement()) {
            if (xml.name() != u"filter")
                fail("Unrecognized filter element: " + xml.name().toString());
            if (result.filters.size() >= 65536)
                fail("Too many filters.");
            Filter filter;
            filter.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QMap<QString, QString> attrs;
            for (const auto& attr : xml.attributes())
                attrs.insert(attr.name().toString(), attr.value().toString());
            const auto type = attrs.take("type");
            if (type == "matches_text") {
                filter.pattern = attrs.take("text");
            } else if (type == "marked") {
                filter.kind = Filter::Kind::Marker;
                filter.marker = integer(attrs.take("marker"), 1, 8) - 1;
            } else {
                fail("Unsupported filter type: " + type);
            }
            if (attrs.contains("enabled")) filter.enabled = boolean(attrs.take("enabled"));
            if (attrs.contains("excluding")) filter.excluding = boolean(attrs.take("excluding"));
            if (attrs.contains("case_sensitive")) filter.caseSensitive = boolean(attrs.take("case_sensitive"));
            if (attrs.contains("regex")) filter.regularExpression = boolean(attrs.take("regex"));
            filter.description = attrs.take("description");
            if (attrs.contains("foreColor")) filter.foreground = color(attrs.take("foreColor"));
            if (attrs.contains("backColor")) filter.background = color(attrs.take("backColor"));
            if (attrs.contains("color")) {
                const auto legacy = color(attrs.take("color"));
                if (!filter.background) filter.background = legacy;
            }
            filter.extraAttributes = attrs;
            validateFilter(filter);
            result.filters.push_back(std::move(filter));
            xml.skipCurrentElement();
        }
    }
    while (!xml.atEnd()) xml.readNext();
    checkXml(xml);
    if (!foundFilters)
        fail("The filter file has no filters element.");
    return result;
}

QByteArray writeFilters(const FilterFile& file)
{
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("TextAnalysisTool.NET");
    auto attrs = file.attributes;
    if (!attrs.contains("version")) attrs.insert("version", "2025-11-21");
    attrs.insert("showOnlyFilteredLines", file.filteredOnly ? "True" : "False");
    for (auto it = attrs.cbegin(); it != attrs.cend(); ++it)
        xml.writeAttribute(it.key(), it.value());
    xml.writeStartElement("filters");
    for (const auto& filter : file.filters) {
        validateFilter(filter);
        xml.writeStartElement("filter");
        auto extra = filter.extraAttributes;
        const QStringList reserved{"enabled", "excluding", "description", "foreColor",
            "backColor", "color", "type", "case_sensitive", "regex", "text", "marker"};
        for (const auto& key : reserved) extra.remove(key);
        for (auto it = extra.cbegin(); it != extra.cend(); ++it)
            xml.writeAttribute(it.key(), it.value());
        xml.writeAttribute("enabled", yesNo(filter.enabled));
        xml.writeAttribute("excluding", yesNo(filter.excluding));
        xml.writeAttribute("description", filter.description);
        if (filter.foreground)
            xml.writeAttribute("foreColor", filter.foreground->name().mid(1));
        if (filter.background)
            xml.writeAttribute("backColor", filter.background->name().mid(1));
        if (filter.kind == Filter::Kind::Marker) {
            xml.writeAttribute("type", "marked");
            xml.writeAttribute("marker", QString::number(filter.marker + 1));
        } else {
            xml.writeAttribute("type", "matches_text");
            xml.writeAttribute("case_sensitive", yesNo(filter.caseSensitive));
            xml.writeAttribute("regex", yesNo(filter.regularExpression));
            xml.writeAttribute("text", filter.pattern);
        }
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return bytes;
}

QByteArray readSmallFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
    if (file.size() > 16 * 1024 * 1024) fail("Configuration file exceeds 16 MiB.");
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) fail(file.errorString());
    return bytes;
}

void writeFileAtomically(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail("Cannot save " + path + ": " + file.errorString());
}

FilterFile loadFilters(const QString& path) { return readFilters(readSmallFile(path)); }
void saveFilters(const QString& path, const FilterFile& file) { writeFileAtomically(path, writeFilters(file)); }

void migrateLegacySettings(QSettings& settings, QSettings& legacy)
{
    const auto existing = settings.allKeys();
    if (settings.status() != QSettings::NoError)
        fail("Unable to load TextScope application settings");
    if (!existing.isEmpty()) return;
    const auto keys = legacy.allKeys();
    if (legacy.status() != QSettings::NoError)
        fail("Unable to read legacy application settings");
    for (const auto& key : keys) settings.setValue(key, legacy.value(key));
    if (legacy.status() != QSettings::NoError)
        fail("Unable to read legacy application settings");
    settings.sync();
    if (settings.status() != QSettings::NoError)
        fail("Unable to save migrated TextScope application settings");
}

Preferences loadPreferences(QSettings& settings)
{
    Preferences p;
    p.font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#ifdef Q_OS_WIN
    if (QFontDatabase::families().contains(QStringLiteral("Consolas")))
        p.font.setFamily(QStringLiteral("Consolas"));
    p.font.setPointSizeF(10);
    p.font.setWeight(QFont::Medium);
#endif
    p.font = settings.value("font", p.font).value<QFont>();
    p.encoding = settings.value("encoding", p.encoding).toByteArray();
    p.zoom = qBound(25, settings.value("zoom", 100).toInt(), 400);
    p.tabWidth = qBound(1, settings.value("tabWidth", 4).toInt(), 32);
    p.markerVisibility = qBound(0, settings.value("markerVisibility", 2).toInt(), 2);
    p.lineNumbers = settings.value("lineNumbers", true).toBool();
    p.tooltips = settings.value("tooltips", true).toBool();
    p.copyLineNumbers = settings.value("copyLineNumbers", false).toBool();
    p.confirmReplacement = settings.value("confirmReplacement", true).toBool();
    p.rememberLayout = settings.value("rememberLayout", true).toBool();
    p.fullPathTitle = settings.value("fullPathTitle", false).toBool();
    p.markToggleChanges = settings.value("markToggleChanges", true).toBool();
    p.textColor = settings.value("textColor", p.textColor).value<QColor>();
    p.windowColor = settings.value("windowColor", p.windowColor).value<QColor>();
    p.selectionColor = settings.value("selectionColor", p.selectionColor).value<QColor>();
    p.selectionTextColor = settings.value("selectionTextColor", p.selectionTextColor).value<QColor>();
    return p;
}

void savePreferences(QSettings& settings, const Preferences& p)
{
    settings.setValue("font", p.font);
    settings.setValue("encoding", p.encoding);
    settings.setValue("zoom", p.zoom);
    settings.setValue("tabWidth", p.tabWidth);
    settings.setValue("markerVisibility", p.markerVisibility);
    settings.setValue("lineNumbers", p.lineNumbers);
    settings.setValue("tooltips", p.tooltips);
    settings.setValue("copyLineNumbers", p.copyLineNumbers);
    settings.setValue("confirmReplacement", p.confirmReplacement);
    settings.setValue("rememberLayout", p.rememberLayout);
    settings.setValue("fullPathTitle", p.fullPathTitle);
    settings.setValue("markToggleChanges", p.markToggleChanges);
    settings.setValue("textColor", p.textColor);
    settings.setValue("windowColor", p.windowColor);
    settings.setValue("selectionColor", p.selectionColor);
    settings.setValue("selectionTextColor", p.selectionTextColor);
    settings.sync();
    if (settings.status() != QSettings::NoError) fail("Unable to persist application settings.");
}

QByteArray writePreferences(const Preferences& p)
{
    QByteArray bytes;
    QXmlStreamWriter xml(&bytes);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("TextScope");
    xml.writeAttribute("version", "1");
    const auto option = [&xml](const QString& name, const QString& value) {
        xml.writeStartElement("option");
        xml.writeAttribute("name", name);
        xml.writeAttribute("value", value);
        xml.writeEndElement();
    };
    option("Font", p.font.toString());
    option("DefaultEncodingName", QString::fromLatin1(p.encoding));
    option("ZoomPercent", QString::number(p.zoom));
    option("TabSize", QString::number(p.tabWidth));
    option("ShowMarkers", QString::number(p.markerVisibility));
    option("ShowLineNumbers", yesNo(p.lineNumbers));
    option("ShowFilterToolTip", yesNo(p.tooltips));
    option("CopyLineNumbers", yesNo(p.copyLineNumbers));
    option("ConfirmReplacement", yesNo(p.confirmReplacement));
    option("RememberFilterColumnLayout", yesNo(p.rememberLayout));
    option("FullPathTitle", yesNo(p.fullPathTitle));
    option("MarkToggleChanges", yesNo(p.markToggleChanges));
    option("WindowTextColor", p.textColor.name());
    option("WindowColor", p.windowColor.name());
    option("HighlightColor", p.selectionColor.name());
    option("HighlightTextColor", p.selectionTextColor.name());
    xml.writeEndElement();
    xml.writeEndDocument();
    return bytes;
}

Preferences readPreferences(const QByteArray& bytes, Preferences p)
{
    QXmlStreamReader xml(bytes);
    if (!xml.readNextStartElement() ||
        (xml.name() != u"TextScope" && xml.name() != u"TextAnalysisTool.Qt"))
        fail("Unsupported preferences schema. Use a TextScope XML export.");
    while (xml.readNextStartElement()) {
        if (xml.name() != u"option") fail("Unrecognized preferences element.");
        const auto name = xml.attributes().value("name").toString();
        const auto value = xml.attributes().value("value").toString();
        if (name == "Font") {
            if (!p.font.fromString(value)) fail("Invalid font.");
        } else if (name == "DefaultEncodingName") p.encoding = value.toLatin1();
        else if (name == "ZoomPercent") p.zoom = integer(value, 25, 400);
        else if (name == "TabSize") p.tabWidth = integer(value, 1, 32);
        else if (name == "ShowMarkers") p.markerVisibility = integer(value, 0, 2);
        else if (name == "ShowLineNumbers") p.lineNumbers = boolean(value);
        else if (name == "ShowFilterToolTip") p.tooltips = boolean(value);
        else if (name == "CopyLineNumbers") p.copyLineNumbers = boolean(value);
        else if (name == "ConfirmReplacement") p.confirmReplacement = boolean(value);
        else if (name == "RememberFilterColumnLayout") p.rememberLayout = boolean(value);
        else if (name == "FullPathTitle") p.fullPathTitle = boolean(value);
        else if (name == "MarkToggleChanges") p.markToggleChanges = boolean(value);
        else if (name == "WindowTextColor") p.textColor = color(value);
        else if (name == "WindowColor") p.windowColor = color(value);
        else if (name == "HighlightColor") p.selectionColor = color(value);
        else if (name == "HighlightTextColor") p.selectionTextColor = color(value);
        else fail("Unsupported preferences option: " + name);
        xml.skipCurrentElement();
    }
    while (!xml.atEnd()) xml.readNext();
    checkXml(xml);
    if (!QTextCodec::codecForName(p.encoding))
        fail("Unsupported default encoding: " + QString::fromLatin1(p.encoding));
    return p;
}

QStringList recentFiles(QSettings& settings, const QString& key)
{
    return settings.value(key).toStringList();
}

void rememberFile(QSettings& settings, const QString& key, const QString& path)
{
    auto list = recentFiles(settings, key);
    const auto absolute = QFileInfo(path).absoluteFilePath();
    list.removeAll(absolute);
    list.prepend(absolute);
    while (list.size() > 12) list.removeLast();
    settings.setValue(key, list);
}
}
