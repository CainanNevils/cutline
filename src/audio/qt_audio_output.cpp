// Fallback audio output via Qt Multimedia (used only if WASAPI shared mode cannot start).
#include "audio/audio_output.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QCoreApplication>
#include <QIODevice>
#include <QMediaDevices>
#include <QMetaObject>
#include <QThread>

#include <atomic>

namespace cut {

namespace {

class PullDevice : public QIODevice {
public:
    explicit PullDevice(AudioOutput::PullFn f) : pull_(std::move(f)) {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 48000 * 8; }
    std::atomic<int64_t> frames{0};

protected:
    qint64 readData(char* data, qint64 maxlen) override {
        int n = int(maxlen / 8);
        if (n <= 0) return 0;
        pull_(reinterpret_cast<float*>(data), n);
        frames += n;
        return qint64(n) * 8;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    AudioOutput::PullFn pull_;
};

class QtAudioOutput final : public AudioOutput {
public:
    ~QtAudioOutput() override { stop(); }

    bool start(PullFn pull, std::string& error) override {
        if (!QCoreApplication::instance()) {
            error = "Qt audio: no QCoreApplication";
            return false;
        }
        stop();
        thread_ = new QThread();
        thread_->start();
        bool ok = false;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [&] {
                QAudioFormat fmt;
                fmt.setSampleRate(48000);
                fmt.setChannelCount(2);
                fmt.setSampleFormat(QAudioFormat::Float);
                QAudioDevice dev = QMediaDevices::defaultAudioOutput();
                if (dev.isNull() || !dev.isFormatSupported(fmt)) return;
                name_ = dev.description().toStdString();
                sink_ = new QAudioSink(dev, fmt);
                sink_->setBufferSize(48000 * 8 / 25);  // 40 ms
                io_ = new PullDevice(pull);
                io_->open(QIODevice::ReadOnly);
                sink_->moveToThread(thread_);
                QMetaObject::invokeMethod(sink_, [this] { sink_->start(io_); }, Qt::BlockingQueuedConnection);
                ok = true;
            },
            QThread::currentThread() == QCoreApplication::instance()->thread() ? Qt::DirectConnection
                                                                                : Qt::BlockingQueuedConnection);
        if (!ok) {
            error = "Qt audio: no usable output device";
            stop();
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() override {
        if (sink_) {
            QMetaObject::invokeMethod(sink_, [this] { sink_->stop(); delete sink_; }, Qt::BlockingQueuedConnection);
            sink_ = nullptr;
        }
        delete io_;
        io_ = nullptr;
        if (thread_) {
            thread_->quit();
            thread_->wait();
            delete thread_;
            thread_ = nullptr;
        }
        running_ = false;
    }

    bool running() const override { return running_; }
    int64_t playedFrames() const override {
        if (!io_) return 0;
        int64_t buffered = int64_t(48000 * 0.04);
        return std::max<int64_t>(0, io_->frames.load() - buffered);
    }
    double latencyMs() const override { return 40; }
    double bufferMs() const override { return 40; }
    int64_t prefillFrames() const override { return 0; }
    std::string name() const override { return "Qt: " + name_; }
    uint64_t glitches() const override { return 0; }

private:
    QThread* thread_ = nullptr;
    QAudioSink* sink_ = nullptr;
    PullDevice* io_ = nullptr;
    std::atomic<bool> running_{false};
    std::string name_;
};

} // namespace

std::unique_ptr<AudioOutput> createQtAudioOutput() { return std::make_unique<QtAudioOutput>(); }

} // namespace cut
