#pragma once
#include "core/model.h"

#include <QDialog>

#include <memory>

class QComboBox;
class QGroupBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace cut {
class ExportJob;
struct ExportSettings;
}

namespace cut::ui {

class Session;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(Session* s, QWidget* parent = nullptr);
    ~ExportDialog() override;

protected:
    void closeEvent(QCloseEvent*) override;
    void reject() override;

private:
    void applyPreset(int i);
    bool buildSettings(ExportSettings& es);
    void start();        // queue it and follow its progress here
    void addToQueue();   // queue it and close
    void poll();
    Session* s_;
    QComboBox *preset_, *codec_, *rc_, *nvPreset_, *audioRate_, *range_, *fps_;
    QSpinBox *w_, *h_, *bframes_, *lookahead_, *cq_;
    QDoubleSpinBox *bitrate_, *maxrate_;
    QCheckBox *audio_, *burn_, *srt_, *hq_, *normalize_;
    QComboBox* lufs_;
    QGroupBox* videoGroup_ = nullptr;
    QLineEdit* path_;
    QStackedWidget* pages_;
    QProgressBar* progress_;
    QLabel* status_;
    QPushButton *cancel_, *openFolder_, *exportBtn_;
    QTimer* timer_;
    int queued_ = 0;  // export queue entry followed by the progress page
};

class SpeedDialog : public QDialog {
    Q_OBJECT
public:
    SpeedDialog(Session* s, QWidget* parent = nullptr);
    void apply();

private:
    Session* s_;
    QDoubleSpinBox* speed_;
    QCheckBox *reverse_, *ripple_, *blend_;
    QLabel* duration_;
};

class SequenceSettingsDialog : public QDialog {
    Q_OBJECT
public:
    SequenceSettingsDialog(const SequenceSettings& current, QWidget* parent = nullptr);
    SequenceSettings settings() const;

private:
    QComboBox *size_, *fps_;
    QSpinBox *w_, *h_;
};

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(Session* s, QWidget* parent = nullptr);
    void apply();

private:
    Session* s_;
    QLineEdit* cacheDir_;
    QSpinBox* capGb_;
    QSpinBox* gpuMb_;
    QCheckBox* autoProxy_;
    QComboBox* audioDev_;
    QComboBox* audioIn_ = nullptr;
    QLabel* usage_;
};

} // namespace cut::ui
