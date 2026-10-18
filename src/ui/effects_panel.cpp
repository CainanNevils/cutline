#include "ui/effects_panel.h"

#include "core/edits.h"
#include "core/project_io.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QVBoxLayout>

#include <functional>

namespace cut::ui {

namespace {

constexpr int kKindRole = Qt::UserRole + 1;  // 1 effect, 2 video transition, 3 audio transition, 4 preset
constexpr int kValueRole = Qt::UserRole + 2;

class EffectsTree : public QTreeWidget {
public:
    EffectsTree(Session* s, QWidget* parent) : QTreeWidget(parent), s_(s) {}

protected:
    QStringList mimeTypes() const override {
        return {"application/x-cutline-effect", "application/x-cutline-effect-preset", "application/x-cutline-transition"};
    }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        if (items.isEmpty()) return nullptr;
        auto* md = new QMimeData;
        QTreeWidgetItem* it = items.first();
        const int kind = it->data(0, kKindRole).toInt();
        if (kind == 1) md->setData("application/x-cutline-effect", it->data(0, kValueRole).toString().toUtf8());
        else if (kind == 2 || kind == 3)
            md->setData("application/x-cutline-transition", QByteArray::number(it->data(0, kValueRole).toInt()));
        else if (kind == 4) {
            Effect e;
            if (s_->effectPreset(it->data(0, kValueRole).toString(), e))
                md->setData("application/x-cutline-effect-preset", QByteArray::fromStdString(effectToJson(e)));
        }
        return md;
    }

private:
    Session* s_;
};

QTreeWidgetItem* folder(QTreeWidget* tree, QTreeWidgetItem* parent, const QString& name) {
    auto* it = parent ? new QTreeWidgetItem(parent, {name}) : new QTreeWidgetItem(tree, {name});
    it->setIcon(0, icon("folder", QColor(0xd8, 0xb0, 0x50)));
    return it;
}

} // namespace

EffectsPanel::EffectsPanel(Session* s, QWidget* parent) : QWidget(parent), s_(s) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 0);
    v->setSpacing(4);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search effects"));
    search_->setClearButtonEnabled(true);
    v->addWidget(search_);
    tree_ = new EffectsTree(s, this);
    tree_->setHeaderHidden(true);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setRootIsDecorated(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(tree_, 1);

    presets_ = folder(tree_, nullptr, tr("Presets"));
    auto* looks = folder(tree_, nullptr, tr("Looks"));
    for (const QString& n : Session::lookNames()) {
        auto* it = new QTreeWidgetItem(looks, {n});
        it->setIcon(0, icon("fx", col::accent));
        it->setData(0, kKindRole, 4);
        it->setData(0, kValueRole, "look:" + n);
        it->setToolTip(0, tr("Colour look (Color Correction settings) - drag onto clips or an adjustment layer, or "
                             "double-click to apply to the selected clips; fine-tune in Effect Controls"));
    }
    auto* vfx = folder(tree_, nullptr, tr("Video Effects"));
    std::map<std::string, QTreeWidgetItem*> cats;
    for (const auto& d : effectRegistry()) {
        if (d.builtin || !d.video) continue;
        auto& cat = cats[d.category];
        if (!cat) cat = folder(tree_, vfx, toQ(d.category));
        auto* it = new QTreeWidgetItem(cat, {toQ(d.label)});
        it->setIcon(0, icon("fx"));
        it->setData(0, kKindRole, 1);
        it->setData(0, kValueRole, toQ(d.type));
        it->setToolTip(0, tr("Drag onto a clip, or double-click to apply to the selected clips"));
    }
    auto* afx = folder(tree_, nullptr, tr("Audio Effects"));
    std::map<std::string, QTreeWidgetItem*> acats;
    for (const auto& d : effectRegistry()) {
        if (d.builtin || d.video) continue;
        auto& cat = acats[d.category];
        if (!cat) cat = folder(tree_, afx, toQ(d.category));
        auto* it = new QTreeWidgetItem(cat, {toQ(d.label)});
        it->setIcon(0, icon("wave"));
        it->setData(0, kKindRole, 1);
        it->setData(0, kValueRole, toQ(d.type));
        it->setToolTip(0, tr("Drag onto an audio clip, or double-click to apply to the selected audio clips"));
    }
    auto* vtr = folder(tree_, nullptr, tr("Video Transitions"));
    for (TransitionType t : {TransitionType::CrossDissolve, TransitionType::DipToBlack, TransitionType::DipToWhite,
                             TransitionType::WipeLeft, TransitionType::WipeRight, TransitionType::WipeUp,
                             TransitionType::WipeDown, TransitionType::Slide, TransitionType::Push, TransitionType::CrossZoom,
                             TransitionType::IrisRound, TransitionType::BlurDissolve, TransitionType::Whip}) {
        auto* it = new QTreeWidgetItem(vtr, {transitionName(t)});
        it->setIcon(0, icon("transition"));
        it->setData(0, kKindRole, 2);
        it->setData(0, kValueRole, int(t));
        it->setToolTip(0, tr("Drag onto a cut or clip edge (default: Ctrl+D)"));
    }
    auto* atr = folder(tree_, nullptr, tr("Audio Transitions"));
    auto* cp = new QTreeWidgetItem(atr, {tr("Constant Power")});
    cp->setIcon(0, icon("wave"));
    cp->setData(0, kKindRole, 3);
    cp->setData(0, kValueRole, int(TransitionType::ConstantPower));
    cp->setToolTip(0, tr("Drag onto a cut between audio clips (default: Ctrl+Shift+D)"));
    rebuildPresets();
    tree_->expandAll();

    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) { apply(it); });
    connect(tree_, &QWidget::customContextMenuRequested, this, &EffectsPanel::contextMenu);
    connect(search_, &QLineEdit::textChanged, this, &EffectsPanel::setFilter);
    connect(s_, &Session::effectPresetsChanged, this, &EffectsPanel::rebuildPresets);
}

void EffectsPanel::rebuildPresets() {
    qDeleteAll(presets_->takeChildren());
    const QStringList names = s_->effectPresets();
    for (const QString& n : names) {
        Effect e;
        if (!s_->effectPreset(n, e)) continue;
        const EffectDef* d = findEffectDef(e.type);
        auto* it = new QTreeWidgetItem(presets_, {n});
        it->setIcon(0, icon("fx", col::accent));
        it->setData(0, kKindRole, 4);
        it->setData(0, kValueRole, n);
        it->setToolTip(0, tr("%1 preset - drag onto a clip, or double-click to apply to the selected clips")
                              .arg(d ? toQ(d->label) : toQ(e.type)));
    }
    presets_->setHidden(names.isEmpty() && search_->text().isEmpty());
    if (presets_->childCount() == 0) {
        auto* hint = new QTreeWidgetItem(presets_, {tr("(save one from Effect Controls)")});
        hint->setForeground(0, col::textDim);
        hint->setFlags(Qt::ItemIsEnabled);
    }
    presets_->setExpanded(true);
    setFilter(search_->text());
}

void EffectsPanel::setFilter(const QString& text) {
    if (search_->text() != text) search_->setText(text);
    // a leaf is shown when it matches; a folder when any child is shown or its own name matches
    std::function<bool(QTreeWidgetItem*, bool)> walk = [&](QTreeWidgetItem* it, bool parentMatch) {
        const bool self = text.isEmpty() || it->text(0).contains(text, Qt::CaseInsensitive);
        bool any = false;
        for (int i = 0; i < it->childCount(); ++i) any |= walk(it->child(i), parentMatch || self);
        const bool show = it->childCount() ? (any || (self && !text.isEmpty())) : (self || parentMatch);
        it->setHidden(!show);
        if (show && !text.isEmpty()) it->setExpanded(true);
        return show;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) walk(tree_->topLevelItem(i), false);
    if (text.isEmpty() && presets_->childCount() == 1 && presets_->child(0)->data(0, kKindRole).toInt() == 0)
        presets_->setHidden(true);  // only the placeholder: no presets yet
}

void EffectsPanel::apply(QTreeWidgetItem* it) {
    const int kind = it->data(0, kKindRole).toInt();
    if (kind == 1 || kind == 4) {
        auto ids = s_->selection();
        if (ids.empty()) {
            emit s_->message(tr("Select clips in the timeline first."));
            return;
        }
        Effect e;
        if (kind == 1) {
            const std::string type = toS(it->data(0, kValueRole).toString());
            // a plain effect never replaces one already on the clip
            std::vector<Id> without;
            for (Id id : ids)
                if (const Clip* c = s_->seq().clip(id)) {
                    bool has = false;
                    for (const auto& x : c->effects) has |= x.type == type;
                    if (!has) without.push_back(id);
                }
            s_->applyEffect(without, makeEffect(type), tr("Add Effect"));
        } else if (s_->effectPreset(it->data(0, kValueRole).toString(), e)) {
            if (!s_->applyEffect(ids, e, tr("Apply Preset"))) emit s_->message(tr("The preset does not fit the selected clips."));
        }
    } else if (kind == 2 || kind == 3) {
        s_->applyDefaultTransition(kind == 3, it->data(0, kValueRole).toInt());
    }
}

void EffectsPanel::contextMenu(const QPoint& pos) {
    QTreeWidgetItem* it = tree_->itemAt(pos);
    if (!it || it->data(0, kKindRole).toInt() == 0) return;
    QMenu m(this);
    m.addAction(tr("Apply to Selected Clips"), this, [this, it] { apply(it); });
    if (it->data(0, kKindRole).toInt() == 4) {
        const QString name = it->data(0, kValueRole).toString();
        m.addAction(tr("Rename Preset..."), this, [this, name] {
            bool ok = false;
            const QString n = QInputDialog::getText(this, tr("Rename Preset"), tr("Name:"), QLineEdit::Normal, name, &ok).trimmed();
            Effect e;
            if (!ok || n.isEmpty() || n == name || !s_->effectPreset(name, e)) return;
            s_->removeEffectPreset(name);
            s_->saveEffectPreset(n, e);
        });
        m.addAction(tr("Delete Preset"), this, [this, name] { s_->removeEffectPreset(name); });
    }
    m.exec(tree_->viewport()->mapToGlobal(pos));
}

} // namespace cut::ui
