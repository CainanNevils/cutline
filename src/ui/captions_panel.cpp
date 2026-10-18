#include "ui/captions_panel.h"

#include "core/edits.h"
#include "core/project_io.h"
#include "core/srt.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace cut::ui {

CaptionsPanel::CaptionsPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    auto btn = [&](const QString& text, const QString& tip) {
        auto* b = new QPushButton(text, this);
        b->setToolTip(tip);
        bar->addWidget(b);
        return b;
    };
    auto* add = btn(tr("Add"), tr("Add a caption at the playhead"));
    auto* split = btn(tr("Split"), tr("Split the selected caption at the playhead"));
    auto* merge = btn(tr("Merge"), tr("Merge the selected caption with the next one"));
    auto* del = btn(tr("Delete"), tr("Delete the selected captions"));
    bar->addStretch(1);
    auto* imp = btn(tr("Import SRT..."), tr("Add captions from a .srt/.vtt file"));
    auto* exp = btn(tr("Export SRT..."), tr("Write the captions to a .srt file"));
    auto* sty = btn(tr("Style..."), tr("Caption font, colours, outline and position"));
    v->addLayout(bar);
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("Start"), tr("End"), tr("Text")});
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    v->addWidget(table_, 1);

    connect(add, &QPushButton::clicked, this, [this] { s_->addCaptionAtPlayhead(); });
    connect(split, &QPushButton::clicked, this, [this] {
        for (Id id : s_->selectedCaptions) s_->edit(tr("Split Caption"), [&](Project& p) { edit::splitCaption(p.sequence, id, s_->playhead()); });
    });
    connect(merge, &QPushButton::clicked, this, [this] {
        if (!s_->selectedCaptions.empty()) {
            Id id = s_->selectedCaptions.front();
            s_->edit(tr("Merge Captions"), [&](Project& p) { edit::mergeCaptionWithNext(p.sequence, id); });
        }
    });
    connect(del, &QPushButton::clicked, this, [this] {
        s_->setSelection({});
        s_->deleteSelection(false);
    });
    connect(imp, &QPushButton::clicked, this, &CaptionsPanel::importSrt);
    connect(exp, &QPushButton::clicked, this, &CaptionsPanel::exportSrt);
    connect(sty, &QPushButton::clicked, this, &CaptionsPanel::styleDialog);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (updating_) return;
        std::vector<Id> ids;
        for (const auto& r : table_->selectionModel()->selectedRows()) ids.push_back(Id(table_->item(r.row(), 0)->data(Qt::UserRole).toULongLong()));
        s_->selectedCaptions = ids;
        emit s_->selectionChanged();
        if (ids.size() == 1)
            for (const auto& c : s_->seq().captions)
                if (c.id == ids[0] && !s_->playing()) s_->setPlayhead(c.start);
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (updating_) return;
        Id id = Id(table_->item(it->row(), 0)->data(Qt::UserRole).toULongLong());
        const Rational fps = s_->seq().settings.fps;
        if (it->column() == 2) {
            std::string t = toS(it->text());
            s_->edit(tr("Edit Caption"), [&](Project& p) {
                for (auto& c : p.sequence.captions)
                    if (c.id == id) c.text = t;
            });
        } else {
            Tick t;
            if (!parseTimecode(toS(it->text()), fps, t)) {
                rebuild();
                return;
            }
            bool start = it->column() == 0;
            s_->edit(tr("Caption Timing"), [&](Project& p) {
                for (auto& c : p.sequence.captions)
                    if (c.id == id) {
                        if (start) c.start = std::min(t, c.end - p.sequence.frameDur());
                        else c.end = std::max(t, c.start + p.sequence.frameDur());
                    }
                edit::sortCaptions(p.sequence);
            });
        }
    });
    connect(s_, &Session::changed, this, &CaptionsPanel::rebuild);
    connect(s_, &Session::selectionChanged, this, [this] {
        updating_ = true;
        table_->clearSelection();
        for (int r = 0; r < table_->rowCount(); ++r) {
            Id id = Id(table_->item(r, 0)->data(Qt::UserRole).toULongLong());
            if (std::find(s_->selectedCaptions.begin(), s_->selectedCaptions.end(), id) != s_->selectedCaptions.end())
                table_->selectRow(r);
        }
        updating_ = false;
    });
    rebuild();
}

void CaptionsPanel::rebuild() {
    updating_ = true;
    const auto& caps = s_->seq().captions;
    const Rational fps = s_->seq().settings.fps;
    table_->setRowCount(int(caps.size()));
    for (int i = 0; i < int(caps.size()); ++i) {
        const Caption& c = caps[size_t(i)];
        auto set = [&](int col, const QString& text) {
            QTableWidgetItem* it = table_->item(i, col);
            if (!it) {
                it = new QTableWidgetItem;
                table_->setItem(i, col, it);
            }
            if (it->text() != text) it->setText(text);
        };
        set(0, toQ(formatTimecode(c.start, fps)));
        set(1, toQ(formatTimecode(c.end, fps)));
        set(2, toQ(c.text));
        table_->item(i, 0)->setData(Qt::UserRole, qulonglong(c.id));
    }
    updating_ = false;
}

void CaptionsPanel::importSrt() {
    QString f = QFileDialog::getOpenFileName(this, tr("Import Captions"), {}, tr("Subtitles (*.srt *.vtt)"));
    if (f.isEmpty()) return;
    std::string text, err;
    std::vector<SrtEntry> entries;
    if (!readFileUtf8(toS(f), text) || !parseSrt(text, entries, err)) {
        QMessageBox::warning(this, tr("Import Captions"), err.empty() ? tr("Could not read the file.") : toQ(err));
        return;
    }
    s_->edit(tr("Import Captions"), [&](Project& p) {
        for (const auto& e : entries) edit::addCaption(p.sequence, e.start, e.end, e.text);
    });
    emit s_->message(tr("Imported %1 captions").arg(entries.size()));
}

void CaptionsPanel::exportSrt() {
    QString f = QFileDialog::getSaveFileName(this, tr("Export Captions"), {}, tr("SubRip (*.srt);;WebVTT (*.vtt)"));
    if (f.isEmpty()) return;
    auto entries = captionsToSrt(s_->seq());
    std::string text = f.endsWith(".vtt", Qt::CaseInsensitive) ? writeVtt(entries) : writeSrt(entries);
    if (!writeFileAtomic(toS(f), text)) QMessageBox::warning(this, tr("Export Captions"), tr("Could not write %1").arg(f));
}

void CaptionsPanel::styleDialog() {
    CaptionStyle st = s_->seq().captionStyle;
    QDialog d(this);
    d.setWindowTitle(tr("Caption Style"));
    auto* form = new QFormLayout(&d);
    auto* font = new QFontComboBox(&d);
    font->setCurrentFont(QFont(toQ(st.text.font)));
    auto* size = new QDoubleSpinBox(&d);
    size->setRange(8, 400);
    size->setValue(st.text.size);
    auto* bold = new QCheckBox(tr("Bold"), &d);
    bold->setChecked(st.text.weight >= 600);
    uint32_t fill = st.text.fill, outline = st.text.strokeColor, box = st.text.boxColor;
    auto colorBtn = [&](uint32_t* c) {
        auto* b = new QPushButton(&d);
        b->setFixedWidth(60);
        auto paint = [b, c] { b->setStyleSheet(QString("background: %1;").arg(QColor::fromRgba(*c).name())); };
        paint();
        QObject::connect(b, &QPushButton::clicked, &d, [&d, c, paint] {
            QColor col = QColorDialog::getColor(QColor::fromRgba(*c), &d, {}, QColorDialog::ShowAlphaChannel);
            if (col.isValid()) {
                *c = col.rgba();
                paint();
            }
        });
        return b;
    };
    auto* outlineOn = new QCheckBox(tr("Outline"), &d);
    outlineOn->setChecked(st.text.stroke);
    auto* outlineW = new QDoubleSpinBox(&d);
    outlineW->setRange(0, 40);
    outlineW->setValue(st.text.strokeWidth);
    auto* boxOn = new QCheckBox(tr("Background box"), &d);
    boxOn->setChecked(st.text.box);
    auto* boxOp = new QDoubleSpinBox(&d);
    boxOp->setRange(0, 100);
    boxOp->setSuffix(" %");
    boxOp->setValue(st.text.boxOpacity);
    auto* margin = new QDoubleSpinBox(&d);
    margin->setRange(0, 50);
    margin->setSuffix(" %");
    margin->setValue(st.marginBottom);
    form->addRow(tr("Font"), font);
    form->addRow(tr("Size (px)"), size);
    form->addRow("", bold);
    form->addRow(tr("Text colour"), colorBtn(&fill));
    auto* ol = new QHBoxLayout;
    ol->addWidget(outlineOn);
    ol->addWidget(outlineW);
    ol->addWidget(colorBtn(&outline));
    form->addRow(tr("Outline"), ol);
    auto* bl = new QHBoxLayout;
    bl->addWidget(boxOn);
    bl->addWidget(boxOp);
    bl->addWidget(colorBtn(&box));
    form->addRow(tr("Background"), bl);
    form->addRow(tr("Bottom margin"), margin);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    st.text.font = toS(font->currentFont().family());
    st.text.size = size->value();
    st.text.weight = bold->isChecked() ? 700 : 400;
    st.text.fill = fill;
    st.text.stroke = outlineOn->isChecked();
    st.text.strokeWidth = outlineW->value();
    st.text.strokeColor = outline;
    st.text.box = boxOn->isChecked();
    st.text.boxOpacity = boxOp->value();
    st.text.boxColor = box;
    st.marginBottom = margin->value();
    s_->edit(tr("Caption Style"), [&](Project& p) { p.sequence.captionStyle = st; });
}

} // namespace cut::ui
