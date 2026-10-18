// Cutline entry point: logging, crash handler, UI-stall watchdog, theme, session, main window.
#include "engine/media_cache.h"
#include "ui/main_window.h"
#include "ui/session.h"
#include "ui/theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::string g_logPath;
std::mutex g_logMutex;

void logLine(const std::string& line) {
    std::lock_guard<std::mutex> l(g_logMutex);
    FILE* f = nullptr;
    if (_wfopen_s(&f, QString::fromStdString(g_logPath).toStdWString().c_str(), L"a") == 0 && f) {
        std::string ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz").toStdString();
        std::fprintf(f, "%s  %s\n", ts.c_str(), line.c_str());
        std::fclose(f);
    }
}

void qtMessage(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    static const char* names[] = {"debug", "warning", "critical", "fatal", "info"};
    logLine(std::string("[qt ") + names[std::clamp(int(type), 0, 4)] + "] " + msg.toStdString());
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "CRASH: exception 0x%08lX at %p", ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress);
    logLine(buf);
    // stack (module + offset; symbols may be missing in a release build)
    void* frames[48];
    USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);
    for (USHORT i = 0; i < n; ++i) {
        char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        HMODULE mod = nullptr;
        char modName[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &mod);
        if (mod) GetModuleFileNameA(mod, modName, MAX_PATH);
        if (SymFromAddr(proc, DWORD64(frames[i]), &disp, sym))
            std::snprintf(buf, sizeof(buf), "  #%u %s!%s+0x%llx", i, strrchr(modName, '\\') ? strrchr(modName, '\\') + 1 : modName,
                          sym->Name, (unsigned long long)disp);
        else
            std::snprintf(buf, sizeof(buf), "  #%u %s+0x%llx", i, strrchr(modName, '\\') ? strrchr(modName, '\\') + 1 : modName,
                          (unsigned long long)(DWORD64(frames[i]) - DWORD64(mod)));
        logLine(buf);
    }
    // minidump next to the log
    std::string dmp = g_logPath.substr(0, g_logPath.rfind('\\') + 1) + "crash.dmp";
    HANDLE f = CreateFileA(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mi{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(proc, GetCurrentProcessId(), f, MiniDumpNormal, &mi, nullptr, nullptr);
        CloseHandle(f);
    }
    MessageBoxW(nullptr,
                L"Cutline hit an unexpected error and has to close.\n\nYour work is autosaved every minute; "
                L"you will be offered to recover it on the next start.\n\nA crash log was written to "
                L"%LOCALAPPDATA%\\Cutline\\cutline.log",
                L"Cutline", MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Logs whenever the UI thread does not service its event loop for > 50 ms (never blocks it).
class UiWatchdog {
public:
    void start() {
        timer_ = new QTimer();
        QObject::connect(timer_, &QTimer::timeout, [this] { beat_ = now(); });
        timer_->start(20);
        beat_ = now();
        thread_ = std::thread([this] {
            bool stalled = false;
            int64_t stallStart = 0;
            while (!quit_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                int64_t gap = now() - beat_.load();
                if (gap > 70 && !stalled) {
                    stalled = true;
                    stallStart = beat_.load();
                } else if (gap <= 70 && stalled) {
                    stalled = false;
                    int64_t ms = now() - stallStart;
                    if (ms > 80) logLine("UI thread stall: " + std::to_string(ms) + " ms");
                }
            }
        });
    }
    ~UiWatchdog() {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        delete timer_;
    }

private:
    static int64_t now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    QTimer* timer_ = nullptr;
    std::thread thread_;
    std::atomic<int64_t> beat_{0};
    std::atomic<bool> quit_{false};
};

} // namespace

int main(int argc, char** argv) {
    SetUnhandledExceptionFilter(crashHandler);
    g_logPath = cut::appDataDir() + "\\cutline.log";
    std::set_terminate([] {
        logLine("std::terminate called");
        std::abort();
    });
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Cutline");
    QApplication::setApplicationName("Cutline");
    QApplication::setApplicationVersion(CUTLINE_VERSION);
    qInstallMessageHandler(qtMessage);
    logLine("---- Cutline " CUTLINE_VERSION " starting ----");
    cut::ui::applyTheme(app);
    app.setWindowIcon(cut::ui::icon("cutline"));

    QElapsedTimer phase;
    phase.start();
    std::string phases;
    auto mark = [&](const char* name) {
        phases += std::string(phases.empty() ? "" : ", ") + name + " " + std::to_string(phase.restart()) + " ms";
    };
    cut::ui::Session session;
    mark("session");
    QString err;
    if (!session.startEngine(err)) {
        logLine("engine start failed: " + err.toStdString());
        QMessageBox::critical(nullptr, "Cutline",
                              QObject::tr("Cutline needs a DirectX 11 GPU and could not start its video engine:\n\n%1").arg(err));
        return 1;
    }
    {
        const cut::AudioStatus au = session.engine()->audioStatus();
        char buf[512];
        std::snprintf(buf, sizeof(buf), "audio: %s, %s, latency %.1f ms (buffer %.1f ms)%s", au.backend.c_str(), au.device.c_str(),
                      au.latencyMs, au.bufferMs, au.ok ? "" : " - not running");
        logLine(buf);
    }
    UiWatchdog watchdog;  // started once the window is up: building it is not a stall
    int rc = 0;
    {  // the window (and any open dialogs) go away before the session shuts down
    mark("engine");
    cut::ui::MainWindow w(&session);
    mark("window");
    w.resize(1600, 960);
    if (!QSettings().contains("window/geometry")) w.showMaximized();
    else w.show();
    mark("show");
    QTimer::singleShot(0, &w, [&w, &phases, &watchdog] {
        watchdog.start();
        // cold start to interactive: process creation -> first idle event loop pass with the window shown
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        auto toU = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        logLine("startup: interactive after " + std::to_string((toU(now) - toU(created)) / 10000) + " ms (" + phases + ")");
        w.startup();
    });
    // command line / Explorer "Open with": a project, or media files to import (first one opens
    // in the Source monitor). arguments() is Unicode-safe, unlike argv.
    const QStringList args = QApplication::arguments().mid(1);
    if (!args.isEmpty()) {
        QTimer::singleShot(0, &w, [&session, &w, args] {
            if (args.front().endsWith(".cutproj", Qt::CaseInsensitive)) {
                QString e;
                if (!session.openProject(args.front(), e)) QMessageBox::critical(&w, "Cutline", e);
                return;
            }
            session.importFiles(args, 0, [&session](const std::vector<cut::Id>& ids) {
                if (!ids.empty()) session.openInSource(ids.front());
            });
        });
    }
    rc = app.exec();
    }
    session.shutdown();
    logLine("---- exit " + std::to_string(rc) + " ----");
    return rc;
}
