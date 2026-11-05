// CommandPalette — implementation.

#include "LightCpp/App/CommandPalette.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

int fuzzyScore(const QString& needle, const QString& haystack)
{
    if (needle.isEmpty()) {
        return 0;
    }
    if (haystack.isEmpty()) {
        return -1;
    }
    const QString lowerNeedle = needle.toLower();
    const QString lowerHay = haystack.toLower();

    int score = 0;
    int hayIndex = 0;
    int previousMatch = -2;
    for (const QChar ch : lowerNeedle) {
        if (ch.isSpace()) {
            continue;
        }
        const qsizetype found = lowerHay.indexOf(ch, hayIndex);
        if (found < 0) {
            return -1;
        }
        // Consecutive characters are what make "fd" match "Format Document"
        // more strongly than a scattered match.
        if (found == previousMatch + 1) {
            score += 8;
        }
        // A match at the start of a word is the next strongest signal.
        if (found == 0) {
            score += 12;
        } else if (lowerHay.at(found - 1) == QLatin1Char(' ')
                   || lowerHay.at(found - 1) == QLatin1Char('/')
                   || lowerHay.at(found - 1) == QLatin1Char('&')
                   || lowerHay.at(found - 1) == QLatin1Char('.')) {
            score += 6;
        } else {
            score += 1;
        }
        previousMatch = static_cast<int>(found);
        hayIndex = static_cast<int>(found) + 1;
    }
    // A shorter haystack is a better fit for the same subsequence.
    score -= static_cast<int>(haystack.size()) / 8;
    return score;
}

CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Command Palette"));
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::Popup);
    resize(560, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    input_ = new QLineEdit(this);
    input_->setPlaceholderText(tr("Type a command…"));
    layout->addWidget(input_);

    list_ = new QListWidget(this);
    list_->setAlternatingRowColors(true);
    layout->addWidget(list_, 1);

    auto* hint = new QLabel(tr("Enter runs the selection · Esc closes"), this);
    hint->setEnabled(false);
    layout->addWidget(hint);

    for (QAction* action : actions) {
        if (action == nullptr || action->text().isEmpty()) {
            continue;
        }
        Entry entry;
        entry.action = action;
        // The menu text carries an accelerator ampersand; the palette shows
        // the label a user would read.
        entry.label = QString(action->text()).remove(QLatin1Char('&'));
        entry.shortcut = action->shortcut().toString();
        entries_.append(entry);
    };

    connect(input_, &QLineEdit::textChanged, this, &CommandPalette::refresh);
    connect(input_, &QLineEdit::returnPressed, this, &CommandPalette::acceptCurrent);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
        acceptCurrent();
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        list_->setCurrentItem(item);
    });
    refresh(QString());
    input_->setFocus();
}

void CommandPalette::refresh(const QString& needle)
{
    QList<Entry> matched;
    for (Entry entry : entries_) {
        if (!entry.action->isEnabled()) {
            continue;
        }
        const int score = needle.trimmed().isEmpty()
                              ? 0
                              : fuzzyScore(needle, entry.label);
        if (score < 0) {
            continue;
        }
        entry.score = score;
        matched.append(entry);
    }
    std::stable_sort(matched.begin(), matched.end(),
                     [](const Entry& a, const Entry& b) {
                         if (a.score != b.score) {
                             return a.score > b.score;
                         }
                         return a.label < b.label;
                     });
    // Everything is shown when the needle is empty; a search keeps the best
    // 200 so a huge plugin set cannot make the popup slow.
    if (!needle.trimmed().isEmpty() && matched.size() > 200) {
        matched = matched.mid(0, 200);
    }

    list_->clear();
    for (const Entry& entry : matched) {
        auto* item = new QListWidgetItem(
            entry.shortcut.isEmpty() ? entry.label
                                     : QStringLiteral("%1    %2").arg(entry.label, entry.shortcut));
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(
                                        reinterpret_cast<qulonglong>(entry.action)));
        list_->addItem(item);
    }
    if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
}

void CommandPalette::acceptCurrent()
{
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) {
        return;
    }
    chosen_ = reinterpret_cast<QAction*>(
        item->data(Qt::UserRole).value<qulonglong>());
    accept();
}
