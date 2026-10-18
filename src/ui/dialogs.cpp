#include "ui/dialogs.h"
#include "ui/export_queue.h"

#include "audio/recorder.h"

#include "audio/audio_output.h"
#include "core/edits.h"
#include "export/encoder.h"
#include "export/export_job.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace cut::ui {

namespace {
struct Preset {
    const char* name;
    int w, h;
    const char* codec;
    double mbps, maxMbps;
    double targetMB = 0;  // > 0: bitrate chosen at export so the file fits this size
    const char* audioOnly = nullptr;  // audio-only presets: the audio encoder
    const char* ext = "mp4";
    int audioKbps = 0;
};
const Preset kPresets[] = {
    {"YouTube 1440p60 H.264", 2560, 1440, "h264_nvenc", 24, 36},
    {"YouTube 1080p60 H.264", 1920, 1080, "h264_nvenc", 12, 18},
    {"YouTube 4K (2160p60) HEVC", 3840, 2160, "hevc_nvenc", 45, 65},
    {"Shorts / TikTok 1080x1920", 1080, 1920, "h264_nvenc", 12, 18},
    {"Discord (fits 25 MB, 1080p)", 1920, 1080, "h264_nvenc", 0, 0, 24.0},
    {"HEVC 1440p60", 2560, 1440, "hevc_nvenc", 16, 24},
    {"Match Sequence (H.264)", 0, 0, "h264_nvenc", 20, 30},
    {"YouTube 1440p60 AV1 (smaller files)", 2560, 1440, "av1_nvenc", 16, 24},
    {"Audio only - WAV (lossless)", 0, 0, "", 0, 0, 0, "pcm_s16le", "wav"},
    {"Audio only - MP3 320 kbps", 0, 0, "", 0, 0, 0, "libmp3lame", "mp3", 320},
    {"Audio only - FLAC (lossless)", 0, 0, "", 0, 0, 0, "flac", "flac"},
};
} // namespace

// ---------------------------------------------------------------------------------------------
ExportDialog::ExportDialog(Session* s, QWidget* parent) : QDialog(parent), s_(s) {
    setWindowTitle(tr("Export"));
    setMinimumWidth(560);
    auto* v = new QVBoxLayout(this);
    pages_ = new QStackedWidget(this);
    v->addWidget(pages_);

    // --- settings page ---------------------------------------------------------------------
    auto* settings = new QWidget;
    auto* sv = new QVBoxLayout(settings);
    auto* form = new QFormLayout;
    preset_ = new QComboBox;
    for (const auto& p : kPresets) preset_->addItem(p.name);
    preset_->addItem(tr("Custom"));
    form->addRow(tr("Preset"), preset_);
    auto* pathRow = new QHBoxLayout;
    path_ = new QLineEdit;
    auto* browse = new QPushButton(tr("Browse..."));
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browse);
    form->addRow(tr("Output file"), pathRow);
    range_ = new QComboBox;
    range_->addItems({tr("Entire sequence"), tr("Sequence In to Out")});
    if (s_->seq().inPoint != kInvalidTick || s_->seq().outPoint != kInvalidTick) range_->setCurrentIndex(1);
    form->addRow(tr("Range"), range_);
    sv->addLayout(form);

    auto* vg = new QGroupBox(tr("Video"));
    videoGroup_ = vg;
    auto* vf = new QFormLayout(vg);
    auto* sizeRow = new QHBoxLayout;
    w_ = new QSpinBox;
    h_ = new QSpinBox;
    for (auto* b : {w_, h_}) b->setRange(16, 8192);
    sizeRow->addWidget(w_);
    sizeRow->addWidget(new QLabel("x"));
    sizeRow->addWidget(h_);
    sizeRow->addStretch(1);
    vf->addRow(tr("Size"), sizeRow);
    fps_ = new QComboBox;
    fps_->addItem(tr("Same as sequence (%1)").arg(s_->seq().settings.fps.toDouble(), 0, 'g', 5));
    for (const char* f : {"23.976", "24", "25", "29.97", "30", "50", "59.94", "60"}) fps_->addItem(f);
    vf->addRow(tr("Frame rate"), fps_);
    codec_ = new QComboBox;
    auto gpu = s_->engine() ? s_->engine()->gpu() : nullptr;
    for (const char* c : {"h264_nvenc", "hevc_nvenc", "av1_nvenc", "libx264"})
        if (encoderUsable(c, gpu)) codec_->addItem(c);
    if (codec_->count() == 0) codec_->addItem("libx264");
    vf->addRow(tr("Encoder"), codec_);
    rc_ = new QComboBox;
    rc_->addItems({tr("VBR (target / max bitrate)"), tr("Constant quality")});
    vf->addRow(tr("Rate control"), rc_);
    auto* brRow = new QHBoxLayout;
    bitrate_ = new QDoubleSpinBox;
    maxrate_ = new QDoubleSpinBox;
    for (auto* b : {bitrate_, maxrate_}) {
        b->setRange(1, 400);
        b->setSuffix(" Mbps");
    }
    cq_ = new QSpinBox;
    cq_->setRange(1, 51);
    cq_->setValue(21);
    cq_->setPrefix("CQ ");
    brRow->addWidget(bitrate_);
    brRow->addWidget(maxrate_);
    brRow->addWidget(cq_);
    vf->addRow(tr("Bitrate"), brRow);
    auto* adv = new QHBoxLayout;
    nvPreset_ = new QComboBox;
    nvPreset_->addItems({"p4", "p5", "p6", "p7"});
    nvPreset_->setCurrentText("p5");
    bframes_ = new QSpinBox;
    bframes_->setRange(0, 4);
    bframes_->setValue(2);
    bframes_->setPrefix(tr("B-frames "));
    lookahead_ = new QSpinBox;
    lookahead_->setRange(0, 32);
    lookahead_->setValue(16);
    lookahead_->setPrefix(tr("lookahead "));
    adv->addWidget(nvPreset_);
    adv->addWidget(bframes_);
    adv->addWidget(lookahead_);
    vf->addRow(tr("NVENC"), adv);
    hq_ = new QCheckBox(tr("High-quality compositing (16-bit float)"));
    vf->addRow("", hq_);
    sv->addWidget(vg);

    auto* ag = new QGroupBox(tr("Audio && captions"));
    auto* af = new QFormLayout(ag);
    auto* aRow = new QHBoxLayout;
    audio_ = new QCheckBox(tr("AAC"));
    audio_->setChecked(true);
    audioRate_ = new QComboBox;
    audioRate_->addItems({"192 kbps", "256 kbps", "320 kbps"});
    aRow->addWidget(audio_);
    aRow->addWidget(audioRate_);
    aRow->addStretch(1);
    af->addRow(tr("Audio"), aRow);
    auto* lRow = new QHBoxLayout;
    normalize_ = new QCheckBox(tr("Normalize loudness to"));
    normalize_->setToolTip(tr("Measures the whole mix first, then applies one gain so the export hits the target\n"
                              "(peaks are kept below -1 dBFS). -14 LUFS suits YouTube and most streaming."));
    lufs_ = new QComboBox;
    lufs_->addItems({"-14 LUFS", "-16 LUFS", "-23 LUFS"});
    lRow->addWidget(normalize_);
    lRow->addWidget(lufs_);
    lRow->addStretch(1);
    af->addRow("", lRow);
    connect(audio_, &QCheckBox::toggled, normalize_, &QWidget::setEnabled);
    connect(audio_, &QCheckBox::toggled, lufs_, &QWidget::setEnabled);
    burn_ = new QCheckBox(tr("Burn captions into the video"));
    srt_ = new QCheckBox(tr("Write a sidecar .srt next to the video"));
    const bool hasCaps = !s_->seq().captions.empty();
    burn_->setEnabled(hasCaps);
    srt_->setEnabled(hasCaps);
    af->addRow(tr("Captions"), burn_);
    af->addRow("", srt_);
    sv->addWidget(ag);
    sv->addStretch(1);
    auto* note = new QLabel(tr("Exports always use the original media (never proxies)."));
    note->setStyleSheet("color: #8c8c8c;");
    sv->addWidget(note);
    auto* bb = new QDialogButtonBox;
    auto* queueBtn = bb->addButton(tr("Add to Queue"), QDialogButtonBox::ActionRole);
    queueBtn->setToolTip(tr("Export in the background (Window > Export Queue) and keep editing"));
    exportBtn_ = bb->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    connect(queueBtn, &QPushButton::clicked, this, &ExportDialog::addToQueue);
    sv->addWidget(bb);
    pages_->addWidget(settings);

    // --- progress page ------------------------------------------------------------------------
    auto* prog = new QWidget;
    auto* pv = new QVBoxLayout(prog);
    status_ = new QLabel;
    status_->setWordWrap(true);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    pv->addWidget(status_);
    pv->addWidget(progress_);
    auto* pb = new QHBoxLayout;
    pb->addStretch(1);
    openFolder_ = new QPushButton(tr("Open Folder"));
    openFolder_->setVisible(false);
    cancel_ = new QPushButton(tr("Cancel"));
    pb->addWidget(openFolder_);
    pb->addWidget(cancel_);
    pv->addLayout(pb);
    pv->addStretch(1);
    pages_->addWidget(prog);

    connect(preset_, &QComboBox::currentIndexChanged, this, &ExportDialog::applyPreset);
    connect(rc_, &QComboBox::currentIndexChanged, this, [this](int i) {
        bitrate_->setVisible(i == 0);
        maxrate_->setVisible(i == 0);
        cq_->setVisible(i == 1);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getSaveFileName(this, tr("Export To"), path_->text(), tr("MP4 (*.mp4);;Matroska (*.mkv)"));
        if (!f.isEmpty()) path_->setText(QDir::toNativeSeparators(f));
    });
    connect(bb, &QDialogButtonBox::accepted, this, &ExportDialog::start);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        const ExportQueue::Entry* e = s_->exportQueue()->entry(queued_);
        if (e && (e->state == ExportQueue::State::Running || e->state == ExportQueue::State::Waiting)) s_->exportQueue()->cancel(queued_);
        else QDialog::reject();
    });
    connect(openFolder_, &QPushButton::clicked, this, [this] {
        QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path_->text())});
    });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &ExportDialog::poll);

    QSettings st;
    QString dir = st.value("export/dir", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString();
    // more than one sequence: name the file after the sequence too
    QString base = s_->displayName();
    if (!s_->project().otherSequences.empty()) base += " - " + toQ(s_->seq().name);
    path_->setText(QDir::toNativeSeparators(dir + "/" + base + ".mp4"));
    preset_->setCurrentIndex(st.value("export/preset", 0).toInt());
    // a vertical sequence starts from the vertical preset
    if (s_->seq().settings.height > s_->seq().settings.width) {
        const int pi = preset_->currentIndex();
        if (pi >= int(std::size(kPresets)) || kPresets[pi].h <= kPresets[pi].w)
            for (int i = 0; i < int(std::size(kPresets)); ++i)
                if (kPresets[i].h > kPresets[i].w) preset_->setCurrentIndex(i);
    }
    normalize_->setChecked(st.value("export/normalize", false).toBool());
    lufs_->setCurrentIndex(st.value("export/normalizeTarget", 0).toInt());
    applyPreset(preset_->currentIndex());
    rc_->setCurrentIndex(0);
    cq_->setVisible(false);
}

ExportDialog::~ExportDialog() = default;

void ExportDialog::applyPreset(int i) {
    const int n = int(std::size(kPresets));
    if (i < 0 || i >= n) return;
    const Preset& p = kPresets[i];
    const auto& seq = s_->seq().settings;
    // audio-only presets switch the picture off and the file type over
    videoGroup_->setEnabled(!p.audioOnly);
    if (p.audioOnly) audio_->setChecked(true);
    audio_->setEnabled(!p.audioOnly);
    burn_->setEnabled(!p.audioOnly && !s_->seq().captions.empty());
    {
        QString path = path_->text();
        const int dot = path.lastIndexOf('.');
        const QString base = dot > path.lastIndexOf(QRegularExpression("[\\\\/]")) ? path.left(dot) : path;
        const QString suffix = dot > 0 ? path.mid(dot + 1).toLower() : QString();
        const bool audioSuffix = suffix == "wav" || suffix == "mp3" || suffix == "flac" || suffix == "m4a";
        if (p.audioOnly) path_->setText(base + "." + p.ext);
        else if (audioSuffix) path_->setText(base + ".mp4");
    }
    if (p.audioOnly) return;
    w_->setValue(p.w ? p.w : seq.width);
    h_->setValue(p.h ? p.h : seq.height);
    int ci = codec_->findText(p.codec);
    codec_->setCurrentIndex(ci >= 0 ? ci : codec_->findText("libx264"));
    if (p.targetMB > 0) {  // shown as an estimate; the real value is computed from the length at export
        const double secs = std::max(0.5, ticksToSeconds(s_->seq().duration()));
        const double mbps = std::max(0.2, (p.targetMB * 8 * 0.97 - (audio_->isChecked() ? 0.128 * secs : 0)) / secs);
        bitrate_->setValue(mbps);
        maxrate_->setValue(mbps * 1.2);
        rc_->setCurrentIndex(0);
    } else {
        bitrate_->setValue(p.mbps);
        maxrate_->setValue(p.maxMbps);
    }
}

bool ExportDialog::buildSettings(ExportSettings& es) {
    es.path = toS(path_->text());
    if (es.path.empty()) return false;
    QFileInfo fi(path_->text());
    if (fi.suffix().isEmpty()) {
        path_->setText(path_->text() + ".mp4");
        es.path = toS(path_->text());
    }
    if (QFileInfo::exists(path_->text()) &&
        QMessageBox::question(this, tr("Export"), tr("%1 exists. Replace it?").arg(fi.fileName())) != QMessageBox::Yes)
        return false;
    const Sequence& seq = s_->seq();
    es.width = w_->value() & ~1;
    es.height = h_->value() & ~1;
    if (fps_->currentIndex() > 0) {
        const Rational rates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
        es.fps = rates[fps_->currentIndex() - 1];
    }
    es.rangeStart = 0;
    es.rangeEnd = seq.duration();
    if (range_->currentIndex() == 1) {
        if (seq.inPoint != kInvalidTick) es.rangeStart = seq.inPoint;
        if (seq.outPoint != kInvalidTick) es.rangeEnd = seq.outPoint;
    }
    if (es.rangeEnd <= es.rangeStart) {
        QMessageBox::warning(this, tr("Export"), tr("The sequence (or In/Out range) is empty."));
        return false;
    }
    es.video.codec = toS(codec_->currentText());
    es.video.preset = toS(nvPreset_->currentText());
    es.video.bframes = bframes_->value();
    es.video.lookahead = lookahead_->value();
    es.video.gop = int(std::lround((es.fps.valid() ? es.fps : seq.settings.fps).toDouble() * 2));
    if (rc_->currentIndex() == 1) {
        es.video.cq = cq_->value();
        es.video.maxBitrate = int64_t(maxrate_->value() * 1e6);
    } else {
        es.video.bitrate = int64_t(bitrate_->value() * 1e6);
        es.video.maxBitrate = int64_t(std::max(maxrate_->value(), bitrate_->value()) * 1e6);
    }
    if (const int pi = preset_->currentIndex(); pi >= 0 && pi < int(std::size(kPresets)) && kPresets[pi].targetMB > 0 &&
                                                    rc_->currentIndex() == 0) {
        // whole file under the size: video gets what audio and ~3 % container overhead leave
        const double secs = std::max(0.5, ticksToSeconds(es.rangeEnd - es.rangeStart));
        const double audioBits = audio_->isChecked() ? 128000.0 * secs : 0;
        const double vbps = std::max(200000.0, (kPresets[pi].targetMB * 8e6 * 0.97 - audioBits) / secs);
        es.video.bitrate = int64_t(vbps);
        es.video.maxBitrate = int64_t(vbps * 1.2);
        audioRate_->setCurrentIndex(0);
    }
    es.audio = audio_->isChecked();
    es.audioBitrate = (audioRate_->currentIndex() == 0 ? 192 : audioRate_->currentIndex() == 1 ? 256 : 320) * 1000;
    if (const int pi = preset_->currentIndex(); pi >= 0 && pi < int(std::size(kPresets)) && kPresets[pi].targetMB > 0)
        es.audioBitrate = 128000;
    if (normalize_->isChecked()) es.normalizeLufs = lufs_->currentIndex() == 0 ? -14 : lufs_->currentIndex() == 1 ? -16 : -23;
    es.burnCaptions = burn_->isChecked();
    es.sidecarSrt = srt_->isChecked();
    es.highQuality = hq_->isChecked();
    if (const int pi = preset_->currentIndex(); pi >= 0 && pi < int(std::size(kPresets)) && kPresets[pi].audioOnly) {
        es.withVideo = false;
        es.audio = true;
        es.audioCodec = kPresets[pi].audioOnly;
        es.audioBitrate = kPresets[pi].audioKbps * 1000;
        es.burnCaptions = false;
        es.sidecarSrt = false;
    }
    uint64_t need = 0, free = 0;
    if (!ExportJob::enoughDiskSpace(es, need, free) &&
        QMessageBox::warning(this, tr("Export"),
                             tr("The output drive may not have enough free space (needs about %1 MB, %2 MB free). Export anyway?")
                                 .arg(need >> 20)
                                 .arg(free >> 20),
                             QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return false;
    QSettings st;
    st.setValue("export/dir", fi.absolutePath());
    st.setValue("export/preset", preset_->currentIndex());
    st.setValue("export/normalize", normalize_->isChecked());
    st.setValue("export/normalizeTarget", lufs_->currentIndex());
    // the same file twice in the queue would overwrite itself
    for (const auto& e : s_->exportQueue()->entries())
        if ((e->state == ExportQueue::State::Waiting || e->state == ExportQueue::State::Running) &&
            QFileInfo(toQ(e->settings.path)) == QFileInfo(path_->text())) {
            QMessageBox::warning(this, tr("Export"), tr("%1 is already in the export queue.").arg(fi.fileName()));
            return false;
        }
    return true;
}

void ExportDialog::start() {
    ExportSettings es;
    if (!buildSettings(es)) return;
    queued_ = s_->exportQueue()->add(s_->snapshot(true), es, toQ(s_->seq().name));
    pages_->setCurrentIndex(1);
    status_->setText(tr("Exporting to %1 ...").arg(path_->text()));
    timer_->start(200);
    poll();
}

void ExportDialog::addToQueue() {
    ExportSettings es;
    if (!buildSettings(es)) return;
    s_->exportQueue()->add(s_->snapshot(true), es, toQ(s_->seq().name));
    emit s_->message(tr("Queued %1 - progress in Window > Export Queue.").arg(QFileInfo(path_->text()).fileName()), 6000);
    accept();
}


void ExportDialog::poll() {
    const ExportQueue::Entry* e = s_->exportQueue()->entry(queued_);
    if (!e) {
        timer_->stop();
        return;
    }
    progress_->setValue(int(e->progress * 1000));
    if (e->state == ExportQueue::State::Waiting) {
        status_->setText(tr("Waiting for %n export(s) ahead in the queue...", "", s_->exportQueue()->waitingBefore(queued_)));
        return;
    }
    status_->setText(e->detail);
    if (e->state == ExportQueue::State::Running) return;
    timer_->stop();
    openFolder_->setVisible(e->state == ExportQueue::State::Done);
    cancel_->setText(tr("Close"));
}

void ExportDialog::closeEvent(QCloseEvent* e) {
    if (const ExportQueue::Entry* q = s_->exportQueue()->entry(queued_);
        q && (q->state == ExportQueue::State::Running || q->state == ExportQueue::State::Waiting))
        emit s_->message(tr("The export continues in the background - Window > Export Queue."), 6000);
    QDialog::closeEvent(e);
}

void ExportDialog::reject() {
    if (const ExportQueue::Entry* q = s_->exportQueue()->entry(queued_);
        q && (q->state == ExportQueue::State::Running || q->state == ExportQueue::State::Waiting))
        emit s_->message(tr("The export continues in the background - Window > Export Queue."), 6000);
    QDialog::reject();
}

// ---------------------------------------------------------------------------------------------
SpeedDialog::SpeedDialog(Session* s, QWidget* parent) : QDialog(parent), s_(s) {
    setWindowTitle(tr("Clip Speed / Duration"));
    auto* form = new QFormLayout(this);
    speed_ = new QDoubleSpinBox;
    speed_->setRange(10, 1000);
    speed_->setDecimals(1);
    speed_->setSuffix(" %");
    reverse_ = new QCheckBox(tr("Reverse speed"));
    ripple_ = new QCheckBox(tr("Ripple edit, shifting trailing clips"));
    blend_ = new QCheckBox(tr("Frame blending (smoother slow motion)"));
    duration_ = new QLabel;
    form->addRow(tr("Speed"), speed_);
    form->addRow(tr("Duration"), duration_);
    form->addRow("", reverse_);
    form->addRow("", ripple_);
    form->addRow("", blend_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    const Clip* c = s_->seq().clip(s_->primaryClip());
    if (c) {
        speed_->setValue(c->isHold() ? 100 : c->speed * 100);
        reverse_->setChecked(c->reverse);
        blend_->setChecked(c->frameBlend);
    }
    ripple_->setChecked(QSettings().value("speed/ripple", false).toBool());
    auto upd = [this, c] {
        if (!c) return;
        Tick span = c->isHold() ? c->duration : c->sourceSpan();
        Tick d = floorToFrame(Tick(double(span) / (speed_->value() / 100.0)), s_->seq().settings.fps);
        duration_->setText(toQ(formatTimecode(d, s_->seq().settings.fps)));
    };
    connect(speed_, &QDoubleSpinBox::valueChanged, this, upd);
    upd();
}

void SpeedDialog::apply() {
    Id id = s_->primaryClip();
    if (!id) return;
    QSettings().setValue("speed/ripple", ripple_->isChecked());
    double sp = speed_->value() / 100.0;
    bool rev = reverse_->isChecked(), rip = ripple_->isChecked(), blend = blend_->isChecked();
    s_->edit(tr("Speed/Duration"), [&](Project& p) { edit::setSpeed(p.sequence, p.assets, id, sp, rev, rip, blend); });
}

// ---------------------------------------------------------------------------------------------
SequenceSettingsDialog::SequenceSettingsDialog(const SequenceSettings& cur, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Sequence Settings"));
    auto* form = new QFormLayout(this);
    size_ = new QComboBox;
    size_->addItems({"2560 x 1440 (1440p)", "3440 x 1440 (ultrawide)", "1920 x 1080 (1080p)", "3840 x 2160 (4K UHD)",
                     "1280 x 720 (720p)", "1080 x 1920 (vertical)", tr("Custom")});
    w_ = new QSpinBox;
    h_ = new QSpinBox;
    w_->setRange(16, 8192);
    h_->setRange(16, 8192);
    w_->setValue(cur.width);
    h_->setValue(cur.height);
    auto* sz = new QHBoxLayout;
    sz->addWidget(w_);
    sz->addWidget(new QLabel("x"));
    sz->addWidget(h_);
    fps_ = new QComboBox;
    fps_->addItems({"23.976", "24", "25", "29.97", "30", "50", "59.94", "60"});
    const Rational rates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
    for (int i = 0; i < 8; ++i)
        if (rates[i] == cur.fps) fps_->setCurrentIndex(i);
    form->addRow(tr("Frame size"), size_);
    form->addRow("", sz);
    form->addRow(tr("Frame rate"), fps_);
    form->addRow(tr("Audio"), new QLabel(tr("48 kHz stereo")));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    const int sizes[][2] = {{2560, 1440}, {3440, 1440}, {1920, 1080}, {3840, 2160}, {1280, 720}, {1080, 1920}};
    size_->setCurrentIndex(6);
    for (int i = 0; i < 6; ++i)
        if (sizes[i][0] == cur.width && sizes[i][1] == cur.height) size_->setCurrentIndex(i);
    connect(size_, &QComboBox::currentIndexChanged, this, [=](int i) {
        if (i < 6) {
            w_->setValue(sizes[i][0]);
            h_->setValue(sizes[i][1]);
        }
    });
}

SequenceSettings SequenceSettingsDialog::settings() const {
    SequenceSettings s;
    s.width = w_->value() & ~1;
    s.height = h_->value() & ~1;
    const Rational rates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
    s.fps = rates[std::clamp(fps_->currentIndex(), 0, 7)];
    return s;
}

// ---------------------------------------------------------------------------------------------
PreferencesDialog::PreferencesDialog(Session* s, QWidget* parent) : QDialog(parent), s_(s) {
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(520);
    auto* form = new QFormLayout(this);
    QSettings st;
    auto* dirRow = new QHBoxLayout;
    cacheDir_ = new QLineEdit(toQ(s_->cache().dir()));
    auto* browse = new QPushButton(tr("Browse..."));
    dirRow->addWidget(cacheDir_, 1);
    dirRow->addWidget(browse);
    form->addRow(tr("Media cache folder"), dirRow);
    capGb_ = new QSpinBox;
    capGb_->setRange(1, 2000);
    capGb_->setSuffix(" GB");
    capGb_->setValue(int(s_->cache().capBytes() >> 30));
    form->addRow(tr("Cache size limit"), capGb_);
    auto* useRow = new QHBoxLayout;
    usage_ = new QLabel(tr("%1 MB used").arg(s_->cache().usageBytes() >> 20));
    auto* clear = new QPushButton(tr("Clean Cache"));
    useRow->addWidget(usage_, 1);
    useRow->addWidget(clear);
    form->addRow("", useRow);
    gpuMb_ = new QSpinBox;
    gpuMb_->setRange(512, 16384);
    gpuMb_->setSingleStep(256);
    gpuMb_->setSuffix(" MB");
    gpuMb_->setValue(st.value("gpu/budgetMB", 3072).toInt());
    gpuMb_->setToolTip(tr("GPU memory for decoded preview frames (lower it on 6-8 GB cards if other apps need VRAM)"));
    form->addRow(tr("Preview frame cache"), gpuMb_);
    autoProxy_ = new QCheckBox(tr("Create proxies automatically for media above 1080p"));
    autoProxy_->setChecked(st.value("proxy/auto", true).toBool());
    form->addRow(tr("Proxies"), autoProxy_);
    audioDev_ = new QComboBox;
    audioDev_->addItem(tr("System default"), QString());
    for (const auto& [id, name] : listAudioDevices()) audioDev_->addItem(toQ(name), toQ(id));
    const int di = audioDev_->findData(st.value("audio/device").toString());
    audioDev_->setCurrentIndex(std::max(0, di));
    if (const AudioStatus au = s_->engine() ? s_->engine()->audioStatus() : AudioStatus{}; au.ok)
        audioDev_->setToolTip(tr("Now playing on: %1 (latency %2 ms)").arg(toQ(au.device)).arg(au.latencyMs, 0, 'f', 1));
    form->addRow(tr("Audio output"), audioDev_);
    audioIn_ = new QComboBox;
    audioIn_->addItem(tr("System default microphone"), QString());
    for (const auto& [id, name] : listCaptureDevices()) audioIn_->addItem(toQ(name), toQ(id));
    audioIn_->setCurrentIndex(std::max(0, audioIn_->findData(st.value("audio/input").toString())));
    audioIn_->setToolTip(tr("Used by Sequence > Record Voice-over"));
    form->addRow(tr("Voice-over input"), audioIn_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, tr("Media Cache Folder"), cacheDir_->text());
        if (!d.isEmpty()) cacheDir_->setText(QDir::toNativeSeparators(d));
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Clean Cache"),
                                  tr("Delete all cached proxies, thumbnails and waveforms? They will be rebuilt as needed.")) !=
            QMessageBox::Yes)
            return;
        s_->cache().clearAll();
        usage_->setText(tr("%1 MB used").arg(s_->cache().usageBytes() >> 20));
    });
}

void PreferencesDialog::apply() {
    QSettings st;
    st.setValue("cache/dir", cacheDir_->text() == toQ(defaultCacheDir()) ? QString() : cacheDir_->text());
    st.setValue("cache/capGB", capGb_->value());
    st.setValue("proxy/auto", autoProxy_->isChecked());
    st.setValue("gpu/budgetMB", gpuMb_->value());
    const QString dev = audioDev_->currentData().toString();
    const bool devChanged = dev != st.value("audio/device").toString();
    st.setValue("audio/device", dev);
    st.setValue("audio/input", audioIn_->currentData().toString());
    s_->applyPreferences();
    if (devChanged) {
        QString err;
        if (!s_->restartEngine(err)) QMessageBox::warning(this, tr("Audio output"), err);
    }
    s_->cache().purge();
}

} // namespace cut::ui
