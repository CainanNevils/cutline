#include "ui/shortcuts_dialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace cut::ui {

namespace {
QString label(const QAction* a) {
    QString t = a->text();
    t.remove('&');
    if (t.endsWith("...")) t.chop(3);
    return t;
}
QKeySequence defaultOf(const QAction* a) { return a->property("defaultShortcut").value<QKeySequence>(); }
} // namespace

void applyShortcutOverrides(const QList<QAction*>& actions) {
    QSettings st;
    st.beginGroup("shortcuts");
    for (QAction* a : actions) {
        if (!a->objectName().startsWith("cmd.")) continue;
        if (!a->property("defaultShortcut").isValid()) a->setProperty("defaultShortcut", a->shortcut());
        if (st.contains(a->objectName())) a->setShortcut(QKeySequence(st.value(a->objectName()).toString()));
    }
}

ShortcutsDialog::ShortcutsDialog(QList<QAction*> actions, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    resize(620, 720);
    for (QAction* a : actions)
        if (a->objectName().startsWith("cmd.") && !label(a).isEmpty()) actions_ << a;
    std::sort(actions_.begin(), actions_.end(), [](QAction* a, QAction* b) { return label(a).localeAwareCompare(label(b)) < 0; });
    auto* v = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("Click a shortcut and press the new key. Conflicting shortcuts are shown in red."), this);
    hint->setStyleSheet("color: #8c8c8c;");
    v->addWidget(hint);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Search commands"));
    filter_->setClearButtonEnabled(true);
    v->addWidget(filter_);
    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut"), QString()});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->setColumnWidth(1, 200);
    table_->setColumnWidth(2, 34);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    v->addWidget(table_, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* resetAll = bb->addButton(tr("Reset All to Defaults"), QDialogButtonBox::ResetRole);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (int r = 0; r < table_->rowCount(); ++r)
            if (auto* ed = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(r, 1)))
                ed->setKeySequence(defaultOf(actions_[table_->item(r, 0)->data(Qt::UserRole).toInt()]));
        markConflicts();
    });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& f) {
        for (int r = 0; r < table_->rowCount(); ++r)
            table_->setRowHidden(r, !f.isEmpty() && !table_->item(r, 0)->text().contains(f, Qt::CaseInsensitive));
    });
    rebuild();
}

void ShortcutsDialog::rebuild() {
    table_->setRowCount(int(actions_.size()));
    for (int i = 0; i < actions_.size(); ++i) {
        QAction* a = actions_[i];
        auto* it = new QTableWidgetItem(label(a));
        it->setData(Qt::UserRole, i);
        it->setFlags(Qt::ItemIsEnabled);
        table_->setItem(i, 0, it);
        auto* ed = new QKeySequenceEdit(a->shortcut(), table_);
        ed->setMaximumSequenceLength(1);
        ed->setClearButtonEnabled(true);
        table_->setCellWidget(i, 1, ed);
        connect(ed, &QKeySequenceEdit::keySequenceChanged, this, [this] { markConflicts(); });
        auto* reset = new QToolButton(table_);
        reset->setText(QString::fromUtf8("\xE2\x86\xBA"));
        reset->setToolTip(tr("Default: %1").arg(defaultOf(a).toString(QKeySequence::NativeText)));
        connect(reset, &QToolButton::clicked, this, [this, ed, a] {
            ed->setKeySequence(defaultOf(a));
            markConflicts();
        });
        table_->setCellWidget(i, 2, reset);
    }
    markConflicts();
}

void ShortcutsDialog::markConflicts() {
    QMap<QString, int> count;
    for (int r = 0; r < table_->rowCount(); ++r)
        if (auto* ed = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(r, 1)))
            if (!ed->keySequence().isEmpty()) count[ed->keySequence().toString()]++;
    for (int r = 0; r < table_->rowCount(); ++r) {
        auto* ed = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(r, 1));
        bool clash = ed && !ed->keySequence().isEmpty() && count.value(ed->keySequence().toString()) > 1;
        table_->item(r, 0)->setForeground(clash ? QColor(0xe0, 0x50, 0x40) : palette().color(QPalette::Text));
        table_->item(r, 0)->setToolTip(clash ? tr("This shortcut is used by another command") : QString());
    }
}

void ShortcutsDialog::apply() {
    QSettings st;
    st.beginGroup("shortcuts");
    for (int r = 0; r < table_->rowCount(); ++r) {
        QAction* a = actions_[table_->item(r, 0)->data(Qt::UserRole).toInt()];
        auto* ed = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(r, 1));
        QKeySequence k = ed->keySequence();
        a->setShortcut(k);
        if (k == defaultOf(a)) st.remove(a->objectName());
        else st.setValue(a->objectName(), k.toString());
    }
}

} // namespace cut::ui
