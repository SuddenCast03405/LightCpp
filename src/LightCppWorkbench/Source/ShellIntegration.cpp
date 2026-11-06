#include "LightCpp/Workbench/ShellIntegration.hpp"

namespace LightCpp::Workbench::Shell {

namespace {

/// Split "A" / "D;1" into its letter and the rest.
bool splitMark(const QString& payload, QChar* letter, QString* rest)
{
    const QString trimmed = payload.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    *letter = trimmed.at(0);
    const int semicolon = int(trimmed.indexOf(QLatin1Char(';')));
    *rest = semicolon >= 0 ? trimmed.mid(semicolon + 1) : QString();
    return true;
}

}  // namespace

bool parseOsc133(const QString& payload, int row, Mark* mark)
{
    if (mark == nullptr) {
        return false;
    }
    // The payload arrives as everything after "133;", but a shell may also
    // send the whole "133;A" if the caller did not split it. Both are
    // accepted: the cost is one startsWith.
    QString body = payload;
    if (body.startsWith(QLatin1String("133;"))) {
        body.remove(0, 4);
    }
    QChar letter;
    QString rest;
    if (!splitMark(body, &letter, &rest)) {
        return false;
    }
    Mark parsed;
    parsed.row = row;
    switch (letter.unicode()) {
    case 'A':
        parsed.kind = Mark::Kind::PromptStart;
        break;
    case 'B':
        parsed.kind = Mark::Kind::PromptEnd;
        break;
    case 'C':
        parsed.kind = Mark::Kind::CommandStart;
        break;
    case 'D': {
        parsed.kind = Mark::Kind::CommandEnd;
        // "D;0" is success; "D" alone means the shell did not say. Reading a
        // missing code as 0 would call every failure a success.
        if (!rest.isEmpty()) {
            bool ok = false;
            const int code = rest.section(QLatin1Char(';'), 0, 0).toInt(&ok);
            parsed.exitCode = ok ? code : -1;
        }
        break;
    }
    default:
        // `OSC 133;P` (properties), `133;L` and the oh-my-zsh extensions are
        // not marks this tracker uses.
        return false;
    }
    *mark = parsed;
    return true;
}

void Tracker::setHistoryLimit(int limit)
{
    historyLimit_ = qMax(1, limit);
    while (commands_.size() > historyLimit_) {
        commands_.removeFirst();
    }
}

void Tracker::clear()
{
    marks_.clear();
    commands_.clear();
    promptOpen_ = -1;
    running_ = -1;
    commandRow_ = 0;
}

void Tracker::appendMark(const Mark& mark)
{
    marks_.append(mark);
    while (marks_.size() > markLimit_) {
        marks_.removeFirst();
        // The indices are relative to marks_; keeping them right after a
        // removal is cheaper than searching for the marks again.
        if (promptOpen_ >= 0) {
            --promptOpen_;
            if (promptOpen_ < 0) {
                promptOpen_ = -1;
            }
        }
        if (running_ >= 0) {
            --running_;
            if (running_ < 0) {
                running_ = -1;
            }
        }
    }
}

void Tracker::feed(const Mark& mark)
{
    switch (mark.kind) {
    case Mark::Kind::PromptStart:
        appendMark(mark);
        promptOpen_ = int(marks_.size()) - 1;
        running_ = -1;
        return;
    case Mark::Kind::PromptEnd:
        appendMark(mark);
        return;
    case Mark::Kind::CommandStart:
        appendMark(mark);
        running_ = int(marks_.size()) - 1;
        commandRow_ = mark.row;
        return;
    case Mark::Kind::CommandEnd: {
        appendMark(mark);
        if (running_ < 0) {
            // A `D` with no `C`: the shell was already running a command
            // when the integration was installed. Recording it would produce
            // a command with no start row.
            return;
        }
        CommandRecord record;
        record.promptRow = promptOpen_ >= 0 ? marks_.at(promptOpen_).row : mark.row;
        record.commandRow = commandRow_;
        record.exitCode = mark.exitCode;
        record.finished = true;
        commands_.append(record);
        while (commands_.size() > historyLimit_) {
            commands_.removeFirst();
        }
        running_ = -1;
        return;
    }
    }
}

int Tracker::currentPromptRow() const
{
    return promptOpen_ >= 0 && promptOpen_ < marks_.size()
               ? marks_.at(promptOpen_).row
               : -1;
}

CommandRecord Tracker::lastCommand() const
{
    return commands_.isEmpty() ? CommandRecord{} : commands_.last();
}

CommandRecord Tracker::lastFailure() const
{
    for (int i = commands_.size() - 1; i >= 0; --i) {
        if (commands_.at(i).exitCode > 0) {
            return commands_.at(i);
        }
    }
    return CommandRecord{};
}

QList<int> Tracker::promptRowsBetween(int fromRow, int toRow) const
{
    if (fromRow > toRow) {
        std::swap(fromRow, toRow);
    }
    QList<int> rows;
    for (const Mark& mark : marks_) {
        if (mark.kind == Mark::Kind::PromptStart && mark.row >= fromRow
            && mark.row <= toRow) {
            rows.append(mark.row);
        }
    }
    return rows;
}

QString promptText(const QString& line, int promptEndColumn)
{
    if (line.isEmpty()) {
        return {};
    }
    // Clamping rather than trusting: the mark's column comes from the shell
    // and may exceed the line if the prompt was redrawn after a resize.
    const int cut = qBound(0, promptEndColumn, int(line.size()));
    return line.left(cut);
}

QString commandText(const QString& line, int promptEndColumn)
{
    if (line.isEmpty()) {
        return {};
    }
    const int cut = qBound(0, promptEndColumn, int(line.size()));
    return line.mid(cut);
}

}  // namespace LightCpp::Workbench::Shell
