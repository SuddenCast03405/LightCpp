// CoverageReport — implementation. See the header for the two traps.

#include "LightCpp/Testing/CoverageReport.hpp"

#include <QMap>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace LightCpp::Testing {
namespace {

double percent(int hit, int found)
{
    if (found <= 0) {
        return 0.0;
    }
    return (100.0 * static_cast<double>(hit)) / static_cast<double>(found);
}

}  // namespace

double FileCoverage::linePercent() const
{
    return percent(linesHit, linesFound);
}

double FileCoverage::branchPercent() const
{
    return percent(branchesHit, branchesFound);
}

double CoverageReport::linePercent() const
{
    return percent(linesHit, linesFound);
}

double CoverageReport::branchPercent() const
{
    return percent(branchesHit, branchesFound);
}

QList<FileCoverage> CoverageReport::belowThreshold(double threshold) const
{
    QList<FileCoverage> out;
    for (const FileCoverage& file : files) {
        if (file.linePercent() < threshold) {
            out.append(file);
        }
    }
    std::sort(out.begin(), out.end(), [](const FileCoverage& a, const FileCoverage& b) {
        return a.linePercent() < b.linePercent();
    });
    return out;
}

CoverageReport parseLcov(const QString& text)
{
    CoverageReport report;

    // One pass, one record at a time. The hit count is kept per line and the
    // *maximum* wins, because a merged tracefile repeats a line once per run
    // and a later `DA:n,0` must not "un-execute" an earlier `DA:n,3`.
    FileCoverage current;
    QMap<int, int> hitsPerLine;
    bool inRecord = false;

    const auto flush = [&] {
        if (!inRecord || current.path.isEmpty()) {
            current = FileCoverage{};
            hitsPerLine.clear();
            inRecord = false;
            return;
        }
        if (!hitsPerLine.isEmpty()) {
            if (current.linesFound == 0) {
                current.linesFound = static_cast<int>(hitsPerLine.size());
            }
            int hit = 0;
            for (auto it = hitsPerLine.cbegin(); it != hitsPerLine.cend(); ++it) {
                if (it.value() > 0) {
                    ++hit;
                } else {
                    current.uncoveredLines << it.key();
                }
            }
            if (current.linesHit == 0) {
                current.linesHit = hit;
            }
            std::sort(current.uncoveredLines.begin(), current.uncoveredLines.end());
        }
        report.files << current;
        report.linesFound += current.linesFound;
        report.linesHit += current.linesHit;
        report.branchesFound += current.branchesFound;
        report.branchesHit += current.branchesHit;
        current = FileCoverage{};
        hitsPerLine.clear();
        inRecord = false;
    };

    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1String("SF:"))) {
            flush();
            current.path = line.mid(3);
            inRecord = true;
            continue;
        }
        if (line == QLatin1String("end_of_record")) {
            flush();
            continue;
        }
        if (!inRecord) {
            continue;
        }
        if (line.startsWith(QLatin1String("DA:"))) {
            const QStringList parts = line.mid(3).split(QLatin1Char(','));
            if (parts.size() < 2) {
                continue;
            }
            const int number = parts.at(0).toInt();
            const int hits = parts.at(1).toInt();
            const int previous = hitsPerLine.value(number, -1);
            if (hits > previous) {
                hitsPerLine.insert(number, hits);
            }
            continue;
        }
        if (line.startsWith(QLatin1String("LF:"))) {
            current.linesFound = line.mid(3).toInt();
            continue;
        }
        if (line.startsWith(QLatin1String("LH:"))) {
            current.linesHit = line.mid(3).toInt();
            continue;
        }
        if (line.startsWith(QLatin1String("BRF:"))) {
            current.branchesFound = line.mid(4).toInt();
            continue;
        }
        if (line.startsWith(QLatin1String("BRH:"))) {
            current.branchesHit = line.mid(4).toInt();
            continue;
        }
    }
    flush();
    return report;
}

CoverageReport parseGcov(const QString& text, const QString& sourcePath)
{
    CoverageReport report;
    FileCoverage file;
    file.path = sourcePath;

    // "        -:    0:Source:src/a.cpp"
    // "        5:   12:  int x = 0;"
    // "    #####:   14:  if (x) {"
    // "    =====:   16:  inline void f() {}"
    static const QRegularExpression record(
        QStringLiteral("^\\s*(\\S+):\\s*(\\d+):(.*)$"));

    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = record.match(raw);
        if (!match.hasMatch()) {
            continue;
        }
        const QString count = match.captured(1).trimmed();
        const int number = match.captured(2).toInt();
        if (number == 0) {
            // The zero line carries the metadata, including Source:.
            const QString content = match.captured(3);
            if (content.startsWith(QLatin1String("Source:"))) {
                file.path = content.mid(7);
            }
            continue;
        }
        if (count == QLatin1String("-")) {
            continue;  // not executable: a comment, a blank line, a brace
        }
        if (count == QLatin1String("=====")) {
            continue;  // excluded from coverage
        }
        ++file.linesFound;
        if (count == QLatin1String("#####")) {
            file.uncoveredLines << number;
            continue;
        }
        bool ok = false;
        const qlonglong hits = count.toLongLong(&ok);
        if (ok && hits > 0) {
            ++file.linesHit;
        } else {
            file.uncoveredLines << number;
        }
    }
    report.files << file;
    report.linesFound = file.linesFound;
    report.linesHit = file.linesHit;
    return report;
}

QStringList lcovCaptureArguments(const QString& buildDirectory,
                                 const QString& outputFile)
{
    return {QStringLiteral("--capture"),
            QStringLiteral("--directory"), buildDirectory,
            QStringLiteral("--output-file"), outputFile};
}

QStringList lcovRemoveArguments(const QString& inputFile,
                                const QString& outputFile)
{
    return {QStringLiteral("--remove"), inputFile,
            QStringLiteral("/usr/*"),
            QStringLiteral("*/third_party/*"),
            QStringLiteral("*/_deps/*"),
            QStringLiteral("--output-file"), outputFile};
}

QStringList genhtmlArguments(const QString& infoFile,
                             const QString& outputDirectory)
{
    return {infoFile, QStringLiteral("--output-directory"), outputDirectory};
}

QString formatCoverageSummary(const CoverageReport& report)
{
    // "No data" is about counters, not about the file list: a report built
    // from a merged total (or from a caller that only filled the counters)
    // has no per-file rows yet still has something to say. Testing isEmpty()
    // here would print "no coverage data" over a perfectly good 87% line
    // count.
    if (report.linesFound <= 0 && report.branchesFound <= 0) {
        return QStringLiteral("no coverage data");
    }
    QString text = QStringLiteral("%1% lines (%2/%3)")
                       .arg(report.linePercent(), 0, 'f', 1)
                       .arg(report.linesHit)
                       .arg(report.linesFound);
    if (report.branchesFound > 0) {
        text += QStringLiteral(", %1% branches (%2/%3)")
                    .arg(report.branchPercent(), 0, 'f', 1)
                    .arg(report.branchesHit)
                    .arg(report.branchesFound);
    }
    return text;
}

}  // namespace LightCpp::Testing
