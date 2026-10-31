#include <cstdlib>
#include <ctime>
#include <cstring>
#include <chrono>
#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <Windows.h>
# include <io.h>
# include <fcntl.h>
# include <ShlObj.h>
#else
# include <sys/ioctl.h>
# include <unistd.h>
#endif
#include <replxx.hxx>
#include <mutex>
#include <string>
#include "session/io_queue.hpp"
#include "entity/globals.hpp"
#include "utility/misc.hpp"
#include "utility/ansi_colors.hpp"
#include "utility/string_utils.hpp"

namespace bmmo {

    const bool LOWER_THAN_WIN10 =
#ifdef _WIN32
    [] {
        typedef void (WINAPI* RtlGetVersionPtr) (PRTL_OSVERSIONINFOW);
        HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
        if (hMod) {
            auto func = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
            if (func) {
                RTL_OSVERSIONINFOW VersionInformation{}; func(&VersionInformation);
                return VersionInformation.dwMajorVersion < 10;
            };
        }
        return true;
    }(); // no manifest; we cannot use GetVersion or IsWindowsVersionXXXorGreater
#else
    false;
#endif

    const PATH_STRING SHARED_CONFIG_PATH =
#ifdef _WIN32 // local appdata
    [] {
        std::wstring path_str = L".";
        wchar_t* path_pchar{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path_pchar))) {
            path_str = path_pchar;
            CoTaskMemFree(path_pchar);
        }
        return path_str;
    }();
#else
    ".";
#endif


    namespace {
        FILE* log_file = nullptr;
        bool auto_flush = false;

        // Step IOX (world-agreement-gaps-design.md, "As built (step IOX)"):
        // the server's log off its runner, main-loop and console threads.
        // Once a host starts the writer (start_log_writer: the server, with a
        // strand of its own one-thread io_pool), DebugOutput formats a line on
        // the calling thread and hands it to two session/io_queue.hpp queues -
        // the console (stdout, or replxx on an interactive terminal) and the
        // --log file - which the strand writes in call order; nothing on the
        // calling thread touches the disk or the terminal.  Before the start,
        // after the stop, and in every host that never starts it (the retail
        // mod, the mock client, the record parser) a line is written here as
        // before.  Never destroyed: a crash handler or an atexit path may log
        // after static destruction began.
        struct log_writer {
            std::mutex mutex;                 // the append order is the call order
            bool active = false;
            bool tty = false;                 // stdout is an interactive terminal (replxx)
            bmmo::session::io_queue console;
            bmmo::session::io_queue file;     // the --log file (open while log_file is set)
            uint64_t dropped = 0;             // bytes of lines refused by a full queue
            uint64_t next_peak_note = 16ull << 10;
        };
        log_writer& writer() {
            static log_writer* w = new log_writer;
            return *w;
        }
        // A console nobody reads for minutes must not grow without bound
        // (the headless client's log bound, step SIO).
        constexpr uint64_t kLogMaxQueued = 16ull << 20;

        bmmo::session::io_flush file_flush_mode() {
            return auto_flush ? bmmo::session::io_flush::record : bmmo::session::io_flush::none;
        }

        // log_writer::mutex held.
        void append_line(log_writer& w, bmmo::session::io_queue& q, std::string line) {
            if (w.dropped != 0) {
                const std::string notice = "[log] " + std::to_string(w.dropped)
                                         + " bytes of log lines dropped: the log writer's queue was full\n";
                if (q.append(notice)) w.dropped = 0;
            }
            const size_t size = line.size();
            if (!q.append(std::move(line))) w.dropped += size;
            // The queue's high-water mark, said once per factor of four from
            // 16 KiB (a healthy writer takes each line at once; only a stall
            // lets this many wait), so a stalled run shows its bound.
            if (q.peak_queued() >= w.next_peak_note) {
                const uint64_t peak = q.peak_queued();
                while (w.next_peak_note <= peak) w.next_peak_note *= 4;
                q.append("[log] writer queue peak " + std::to_string(peak) + " bytes waiting for the "
                         + (&q == &w.file ? "log file" : "console") + "\n", true);
            }
        }

        // std::localtime returns a pointer to a shared static tm, so two
        // threads formatting a timestamp at once would corrupt each other's.
        void format_local_time(char* buf, size_t size, std::time_t time) {
            std::tm tm_buf{};
#ifdef _WIN32
            if (localtime_s(&tm_buf, &time) != 0) { buf[0] = '\0'; return; }
#else
            if (!localtime_r(&time, &tm_buf)) { buf[0] = '\0'; return; }
#endif
            std::strftime(buf, size, "%m-%d %T", &tm_buf);
        }
    }

    void set_log_file(FILE* file) { log_file = file; }

    void start_log_writer(std::function<void(std::function<void()>)> poster) {
        auto& w = writer();
        std::lock_guard lk(w.mutex);
        if (w.active || !poster) return;
        w.tty = isatty(fileno(stdout)) != 0;
        if (w.tty && !LOWER_THAN_WIN10) {
            // replxx keeps the prompt below the lines; it may be called from
            // any thread (the writer's here).
            w.console.open_sink([](const std::string& chunk) {
                replxx_instance.print("%s", chunk.c_str());
                return true;
            }, stdout, poster, kLogMaxQueued, bmmo::session::io_flush::batch);
        } else {
            w.console.open_stream(stdout, poster, kLogMaxQueued, bmmo::session::io_flush::batch);
        }
        if (log_file) w.file.open_stream(log_file, poster, kLogMaxQueued, file_flush_mode());
        w.active = true;
        // A plain exit() (not a crash) still writes what is queued: atexit
        // runs before the static objects made before main (replxx) go.
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] { stop_log_writer(); });
        }
    }

    void stop_log_writer() {
        auto& w = writer();
        std::lock_guard lk(w.mutex);
        if (!w.active) return;
        w.active = false;
        // Synchronous and bounded: the writer may be stalled on the disk.
        w.console.close_now(std::chrono::milliseconds(2000));
        w.file.close_now(std::chrono::milliseconds(2000));
        fflush(stdout);
    }

    void set_console_log_flush(uint8_t mode) {
        auto& w = writer();
        std::lock_guard lk(w.mutex);
        w.console.set_flush(static_cast<bmmo::session::io_flush>(mode));
    }

    void LogFileOutput(const char* pMsg) {
        if (!log_file)
            return;
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timeStr[15];
        format_local_time(timeStr, sizeof(timeStr), time);
        {
            auto& w = writer();
            std::lock_guard lk(w.mutex);
            if (w.active && w.file.is_open()) {
                append_line(w, w.file, Sprintf("[%s] %s\n", timeStr, pMsg));
                return;
            }
        }
        fprintf(log_file, "[%s] %s\n", timeStr, pMsg);
        // fflush(log);
    }

    void RightTrim(char* text) {
        char* el = std::strchr(text, '\0');
        if (el > text && el[-1] == '\n')
            text[el - text - 1] = '\0';
    }
 
    void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg, int ansiColor) {
        using namespace std;
        // SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        auto time = chrono::system_clock::to_time_t(chrono::system_clock::now());
        char timeStr[15];
        format_local_time(timeStr, sizeof(timeStr), time);

        // Step IOX: with the writer started the line is queued (the file's
        // and the console's, in call order) and nothing here writes; a Bug
        // line drains everything queued, then dies as before.
        {
            auto& w = writer();
            std::unique_lock lk(w.mutex);
            if (w.active) {
                if (log_file && w.file.is_open())
                    append_line(w, w.file, Sprintf("[%s] %s\n", timeStr, pszMsg));
                if (eType != k_ESteamNetworkingSocketsDebugOutputType_Bug) {
                    std::string line;
                    if (!w.tty)
                        line = Sprintf("[%s] %s\n", timeStr, pszMsg);
#ifdef _WIN32
                    else if (LOWER_THAN_WIN10)
                        line = Sprintf("\r[%s] %s\n> ", timeStr, bmmo::string_utils::utf8_to_ansi(pszMsg));
#endif
                    else if (ansiColor == bmmo::ansi::Reset)
                        line = Sprintf("\r[%s] %s\n", timeStr, pszMsg);
                    else
                        line = Sprintf("\r[%s] %s%s\033[m\n", timeStr, bmmo::ansi::get_escape_code(ansiColor), pszMsg);
                    append_line(w, w.console, std::move(line));
                    return;
                }
                lk.unlock();
                stop_log_writer();   // bounded; the file's line is in it
                fprintf(stderr, "\r[%s] %s\n> ", timeStr, pszMsg);
                fflush(stdout);
                fflush(stderr);
                return exit(2);
            }
        }

        if (log_file) {
            fprintf(log_file, "[%s] %s\n", timeStr, pszMsg);
            if (auto_flush) fflush(log_file);
        }

        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Bug) {
            fprintf(stderr, "\r[%s] %s\n> ", timeStr, pszMsg);
            fflush(stdout);
            fflush(stderr);
            if (log_file) fflush(log_file);
            return exit(2);
        }
        else if (!isatty(fileno(stdout))) {
            printf("[%s] %s\n", timeStr, pszMsg);
        }
#ifdef _WIN32
        else if (LOWER_THAN_WIN10) {
            printf("\r[%s] %s\n> ", timeStr, bmmo::string_utils::utf8_to_ansi(pszMsg).c_str());
        }
#endif
        else {
            // printf("\r%10.2f %s\n> ", time * 1e-6, pszMsg);
            // bmmo::replxx_instance.invoke(replxx::Replxx::ACTION::CLEAR_SELF, '\0');
            if (ansiColor == bmmo::ansi::Reset)
                replxx_instance.print("\r[%s] %s\n", timeStr, pszMsg);
            else
                replxx_instance.print("\r[%s] %s%s\033[m\n", timeStr,
                                      bmmo::ansi::get_escape_code(ansiColor).c_str(), pszMsg);
        }
        fflush(stdout);
        // bmmo::replxx_instance.invoke(replxx::Replxx::ACTION::REPAINT, '\0');
    }

    void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
        DebugOutput(eType, pszMsg, bmmo::ansi::Reset);
    }

    void set_auto_flush_log(bool flush) {
        auto_flush = flush;
        auto& w = writer();
        std::lock_guard lk(w.mutex);
        w.file.set_flush(file_flush_mode());   // step IOX: the writer's mode for the log file
    }

    void flush_log() {
        if (!log_file) return;
        {
            // Step IOX: an operator's explicit flush waits for the writer -
            // without holding the log's mutex, so every other thread keeps
            // logging (queueing) while the disk takes its time.  The queue's
            // state is shared under the mutex (flusher), so a stop or
            // close_log on another thread meanwhile cannot free it under us.
            auto& w = writer();
            std::function<void()> flush;
            {
                std::lock_guard lk(w.mutex);
                if (w.active) flush = w.file.flusher();
            }
            if (flush) flush();
        }
        if (fflush(log_file) == 0)
            Printf("Log file flushed successfully.");
    }

    void close_log() {
        if (!log_file) return;
        {
            auto& w = writer();
            std::lock_guard lk(w.mutex);
            w.file.close_now(std::chrono::milliseconds(2000));
        }
        fclose(log_file);
        log_file = nullptr;
    }

    // For signal handlers: another thread may be inside fprintf on this FILE,
    // so closing it would free a stream out from under it. Flushing is the
    // most we can safely attempt before the process dies.  Step IOX: what the
    // log writer still queues is not drained here (a drain allocates and
    // locks, and this handler must stay minimal): SIGTERM / SIGINT lose the
    // writer's queue - empty in a healthy run, a stall's lines under one -
    // as a kill does; the fatal signals' crash drain (step SIO) writes it.
    void flush_log_on_exit() {
        if (log_file) fflush(log_file);
    }
}
