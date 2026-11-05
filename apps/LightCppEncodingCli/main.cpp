// lightcpp-enc - LightC++ encoding detector and UTF-8 converter.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>

#include <cstdio>

#include "LightCpp/Text/Encoding.hpp"
#include "LightCpp/Text/TextFile.hpp"

namespace {

using LightCpp::Text::TextEncoding;

struct Options {
    bool listEncodings = false;
    QString outputPath;
    QStringList files;
};

Options parseOptions(const QCoreApplication& app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "LightC++ encoding detector & UTF-8 converter.\n"
        "Detects the encoding of each input file and prints a summary; "
        "with --out converts to UTF-8 and writes the result.");
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOption({QStringList{"l", "list"},
                      "list supported encodings and exit"});
    parser.addOption({QStringList{"o", "out"},
                      "write the UTF-8 converted text to <file> (single input only)",
                      "file"});
    parser.addPositionalArgument("files", "input file(s)", "<file>...");

    parser.process(app);

    Options options;
    options.listEncodings = parser.isSet("list");
    options.outputPath = parser.value("out");
    options.files = parser.positionalArguments();
    return options;
}

void listEncodings()
{
    std::printf("Supported encodings:\n");
    const TextEncoding all[] = {
        TextEncoding::Utf8,        TextEncoding::Utf16Le,
        TextEncoding::Utf16Be,     TextEncoding::Utf32Le,
        TextEncoding::Utf32Be,     TextEncoding::Gb18030,
        TextEncoding::Gbk,         TextEncoding::Big5,
        TextEncoding::ShiftJis,    TextEncoding::EucKr,
        TextEncoding::Windows1252, TextEncoding::Latin1,
    };
    for (const TextEncoding encoding : all) {
        std::printf("  %-12s %s\n", LightCpp::Text::toName(encoding),
                    LightCpp::Text::isLegacyCodec(encoding)
                        ? "(platform codec)"
                        : "(built-in)");
    }
}

int run(const Options& options)
{
    if (options.listEncodings) {
        listEncodings();
        return 0;
    }
    if (options.files.isEmpty()) {
        std::fprintf(stderr, "error: no input files (see --help)\n");
        return 2;
    }
    if (!options.outputPath.isEmpty() && options.files.size() > 1) {
        std::fprintf(stderr, "error: --out requires exactly one input file\n");
        return 2;
    }

    int failures = 0;
    for (const QString& file : options.files) {
        const LightCpp::Text::TextFileResult result =
            LightCpp::Text::readDetectedTextFile(file);

        if (!result.hasValue()) {
            std::fprintf(stderr, "%s: %s\n", qPrintable(file),
                         LightCpp::Text::toString(result.error()));
            ++failures;
            continue;
        }

        const LightCpp::Text::TextFileContent& content = result.value();
        std::printf("%s: %s%s, %lld bytes, confidence %d/1000 (%s)\n",
                    qPrintable(file),
                    LightCpp::Text::toName(content.encoding),
                    content.hadBom ? " (BOM)" : "",
                    static_cast<long long>(content.byteSize),
                    content.detectionScore,
                    content.detectionReliable ? "reliable" : "heuristic");

        if (!options.outputPath.isEmpty()) {
            QSaveFile out(options.outputPath);
            if (!out.open(QIODevice::WriteOnly)) {
                std::fprintf(stderr, "error: cannot write %s\n",
                             qPrintable(options.outputPath));
                ++failures;
                continue;
            }
            out.write(content.utf8);
            if (!out.commit()) {
                std::fprintf(stderr, "error: failed to commit %s\n",
                             qPrintable(options.outputPath));
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("lightcpp-enc");
    QCoreApplication::setApplicationVersion(LIGHTCPP_VERSION_STRING);

    return run(parseOptions(app));
}
