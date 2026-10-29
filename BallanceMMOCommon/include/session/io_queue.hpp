#pragma once

// Session I/O off the tick thread (step SIO; world-agreement-gaps-design.md,
// "As built (step SIO)").  A file writer the tick thread only hands finished
// bytes to: append() moves them into an in-memory queue under a short lock
// and posts one write task; the task runs on the host's I/O strand and writes
// everything queued, in order, with no flush.  A host disk stall then delays
// the file, never the tick (WG1's smoke, step SMK: NTFS event 170 froze every
// tick thread at once because the journal flushed after every event).
//
// The journal (session/journal.hpp journal_writer), the headless client's log
// and its inbound capture each own one.  The host hands in an io_poster: a
// function that posts a task to ONE strand (tasks run one at a time, in the
// order posted) - an asio strand of the host's thread pool
// (session/io_pool.hpp: the retail mod's existing thread_pool_, the server's
// and the headless client's own one-thread pool).  This header has no asio in
// it: the SimTool's sim-only builds include the journal without asio.
//
// Without a poster the queue writes synchronously on the calling thread, as
// every writer did before SIO (the SimTool, the tests, the retail legacy
// path's journal).
//
// Bounded: past `max_queued` bytes waiting, append() refuses (returns false)
// and never blocks; the owner decides what a refusal means (the journal stops
// recording with a note, the log counts dropped bytes, the capture stops as a
// full disk would).  A write that fails stops the file (failed()).
//
// Threads: append / close / flush may be called from any one thread at a time
// per queue (the log is appended from several threads: the lock covers it);
// the write task runs on the strand.  The file belongs to whoever holds
// io_mutex: the task, a synchronous drain, or a synchronous close.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bmmo::session {

    using io_task = std::function<void()>;
    // Posts a task to one strand of the host's I/O executor.  Empty: no
    // writer thread, the queue writes on the caller's thread.
    using io_poster = std::function<void(io_task)>;

    namespace io_detail {

        struct queue_state {
            // ---- the queue (queue_mutex: held for a push or a swap, never
            // across I/O; timed, so a crash drain never waits forever on a
            // thread that faulted holding it)
            std::timed_mutex queue_mutex;
            std::vector<std::string> pending;
            uint64_t pending_bytes = 0;
            bool scheduled = false;        // a write task is posted and has not taken the queue yet
            bool close_requested = false;  // no append after it; the next task closes the file
            // ---- the file (io_mutex: held by whoever writes it)
            std::timed_mutex io_mutex;
            std::FILE* file = nullptr;
            bool own_file = true;          // false: stdout, never closed
            // fflush after each written batch.  Off everywhere: the user's
            // decision (2026-10-29) is no proactive flush; whether the
            // headless log should be flushed per batch for a live reader
            // (arena_map_test.py's LogWatch) waits on the user (step SIO).
            bool flush_batches = false;
            // A reopen of a path whose previous file is still being written
            // by its strand (the headless client's second session journal):
            // the file is opened on the writer, after `predecessor` is
            // written out and closed - never truncated under an open handle.
            std::filesystem::path open_path;
            size_t open_buffer = 0;
            std::shared_ptr<queue_state> predecessor;
            std::atomic<bool> closed{false};   // finish_file ran
            // ---- fixed after open
            std::filesystem::path path;    // empty for a stream
            io_poster poster;
            uint64_t max_queued = 0;       // 0 = unbounded
            // ---- statistics (atomics: read by the owner on its thread)
            std::atomic<uint64_t> peak_bytes{0};
            std::atomic<uint64_t> batches{0};
            std::atomic<bool> failed{false};

            queue_state() = default;
            queue_state(const queue_state&) = delete;
            queue_state& operator=(const queue_state&) = delete;
            // The net under an abandoned write task (a pool stopped with the
            // task still queued: asio destroys it unrun): the last owner of a
            // file that is still open writes what is queued and closes it.
            // Nobody else holds the state any more, so no lock is needed.
            ~queue_state();
        };

        inline std::FILE* open_for_write(const std::filesystem::path& path) {
#ifdef _WIN32
            return ::_wfopen(path.c_str(), L"wb");
#else
            return std::fopen(path.c_str(), "wb");
#endif
        }

        // A negative timeout waits as long as it takes (the writer thread's
        // own task); the crash and exit drains pass a bound.
        template <class Mutex>
        inline bool lock_within(Mutex& m, std::chrono::milliseconds timeout) {
            if (timeout.count() < 0) {
                m.lock();
                return true;
            }
            return m.try_lock_for(timeout);
        }
        constexpr std::chrono::milliseconds kWait{-1};

        // BMMO_IO_TEST_DELAY_MS (test only, step SIO's stall test): every
        // write task sleeps that long holding the file before it writes, as a
        // stalled WriteFile would.  Never applies to a synchronous queue.
        inline uint32_t test_delay_ms() {
            static const uint32_t value = [] {
                const char* text = std::getenv("BMMO_IO_TEST_DELAY_MS");
                return text ? static_cast<uint32_t>(std::strtoul(text, nullptr, 10)) : 0u;
            }();
            return value;
        }

        // Every queue with a writer thread, for the exit and crash drains.
        // Never destroyed: a crash handler or an atexit path may use it after
        // static destruction began.
        struct queue_registry {
            std::timed_mutex mutex;
            std::vector<std::weak_ptr<queue_state>> queues;
        };
        // Set by the first registration: a process that never had a queue
        // with a writer thread (the retail legacy path) never builds the
        // registry, so a crash handler's drain allocates nothing there.
        inline std::atomic<bool>& registry_used() {
            static std::atomic<bool> used{false};
            return used;
        }
        inline queue_registry& registry() {
            static queue_registry* r = new queue_registry;
            return *r;
        }
        inline void register_queue(const std::shared_ptr<queue_state>& s) {
            registry_used().store(true, std::memory_order_release);
            auto& r = registry();
            std::lock_guard lk(r.mutex);
            std::vector<std::weak_ptr<queue_state>> live;
            live.reserve(r.queues.size() + 1);
            for (auto& w: r.queues)
                if (!w.expired()) live.push_back(std::move(w));
            live.push_back(s);
            r.queues.swap(live);
        }

        // io_mutex held.
        inline void write_chunks(queue_state& s, const std::vector<std::string>& chunks) {
            for (const auto& c: chunks) {
                if (!s.file || s.failed.load(std::memory_order_relaxed)) return;
                if (!c.empty() && std::fwrite(c.data(), 1, c.size(), s.file) != c.size())
                    s.failed.store(true, std::memory_order_relaxed);
            }
        }
        // io_mutex held.
        inline void finish_file(queue_state& s) {
            if (s.file) {
                if (s.own_file) {
                    if (std::fclose(s.file) != 0) s.failed.store(true, std::memory_order_relaxed);
                } else {
                    std::fflush(s.file);
                }
                s.file = nullptr;
            }
            s.closed.store(true, std::memory_order_release);
        }

        inline void drain_locked(queue_state& s, bool flush, bool honour_close, std::chrono::milliseconds timeout);

        // io_mutex held: a deferred open (a reopen of a path still being
        // written) is done here, on the writer: the predecessor's queue is
        // written and its file closed first, then the path is truncated.
        // False: the predecessor's file is held past `timeout` (a drain only;
        // the open waits for a later task).
        inline bool ensure_open_locked(queue_state& s, std::chrono::milliseconds timeout) {
            if (s.open_path.empty()) return true;
            if (s.predecessor) {
                auto& p = *s.predecessor;
                if (!lock_within(p.io_mutex, timeout)) return false;
                drain_locked(p, false, false, timeout);
                finish_file(p);
                p.io_mutex.unlock();
                s.predecessor.reset();
            }
            s.file = open_for_write(s.open_path);
            if (!s.file) s.failed.store(true, std::memory_order_relaxed);
            else if (s.open_buffer) std::setvbuf(s.file, nullptr, _IOFBF, s.open_buffer);
            s.open_path.clear();
            return true;
        }

        // io_mutex held: take the queue and write it; `close` finishes the
        // file when a close was requested; `flush` hands the stdio buffer to
        // the OS (the explicit flush, the exit drain).  `timeout` bounds the
        // queue lock and a deferred open's wait (kWait: no bound).
        inline void drain_locked(queue_state& s, bool flush, bool honour_close, std::chrono::milliseconds timeout) {
            if (!ensure_open_locked(s, timeout)) return;
            std::vector<std::string> chunks;
            bool close = false;
            if (lock_within(s.queue_mutex, timeout)) {
                chunks.swap(s.pending);
                s.pending_bytes = 0;
                close = s.close_requested;
                s.queue_mutex.unlock();
            }
            write_chunks(s, chunks);
            if (honour_close && close) finish_file(s);
            else if (flush && s.file) std::fflush(s.file);
        }

        // The strand's task: one batch, everything queued when it starts.
        inline void run_batch(const std::shared_ptr<queue_state>& s) {
            std::lock_guard io(s->io_mutex);
            std::vector<std::string> chunks;
            bool close = false;
            {
                std::lock_guard q(s->queue_mutex);
                chunks.swap(s->pending);
                s->pending_bytes = 0;
                s->scheduled = false;
                close = s->close_requested;
            }
            if (const uint32_t delay = test_delay_ms())
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            ensure_open_locked(*s, kWait);
            write_chunks(*s, chunks);
            s->batches.fetch_add(1, std::memory_order_relaxed);
            if (close) finish_file(*s);
            else if (s->flush_batches && s->file) std::fflush(s->file);
        }

        inline queue_state::~queue_state() {
            if (!file && !open_path.empty()) {
                // a deferred open that never ran: the predecessor first
                if (predecessor && predecessor->io_mutex.try_lock_for(std::chrono::milliseconds(1000))) {
                    drain_locked(*predecessor, false, false, std::chrono::milliseconds(1000));
                    finish_file(*predecessor);
                    predecessor->io_mutex.unlock();
                }
                predecessor.reset();
                file = open_for_write(open_path);
                open_path.clear();
                if (!file) return;
            }
            if (!file) return;
            for (const auto& c: pending)
                if (!c.empty() && !failed.load(std::memory_order_relaxed)
                    && std::fwrite(c.data(), 1, c.size(), file) != c.size())
                    failed.store(true, std::memory_order_relaxed);
            if (own_file) std::fclose(file);
            else std::fflush(file);
        }

    }

    class io_queue {
    public:
        io_queue() = default;
        ~io_queue() { close_now(std::chrono::milliseconds(2000)); }
        io_queue(const io_queue&) = delete;
        io_queue& operator=(const io_queue&) = delete;

        // A file of our own, truncated.  `poster` empty: synchronous.
        // `buffer_bytes` != 0 sets the stdio buffer.  Opening is a syscall on
        // the caller's thread (once per file; its failure is reported here).
        //
        // A reopen of the path this queue closed last while its strand still
        // has it (a posted close not run yet, a stalled disk): with a writer
        // thread the open is deferred to the writer, which writes the old
        // file out and closes it first - the tick thread neither truncates
        // under the open handle nor waits; an open failure then shows as
        // failed().  Without one the old file is finished here (bounded wait).
        bool open(const std::filesystem::path& path, io_poster poster, uint64_t max_queued, std::string& error,
                  size_t buffer_bytes = 0) {
            close();
            auto previous = closing_.lock();
            closing_.reset();
            if (previous && (previous->closed.load(std::memory_order_acquire) || previous->path != path))
                previous.reset();
            if (previous && poster) {
                attach(nullptr, true, std::move(poster), max_queued, false);
                state_->path = path;
                state_->open_path = path;
                state_->open_buffer = buffer_bytes;
                state_->predecessor = std::move(previous);
                return true;
            }
            if (previous) {
                if (!previous->io_mutex.try_lock_for(std::chrono::milliseconds(2000))) {
                    error = "cannot create " + path.string() + ": its previous file is still being written";
                    return false;
                }
                io_detail::drain_locked(*previous, false, false, std::chrono::milliseconds(2000));
                io_detail::finish_file(*previous);
                previous->io_mutex.unlock();
            }
            std::FILE* f = io_detail::open_for_write(path);
            if (!f) {
                error = "cannot create " + path.string();
                return false;
            }
            if (buffer_bytes) std::setvbuf(f, nullptr, _IOFBF, buffer_bytes);
            attach(f, true, std::move(poster), max_queued, false);
            state_->path = path;
            return true;
        }

        // A stream someone else owns (stdout): never closed, and (the user's
        // decision, step SIO) not flushed per batch - only at the close and
        // the exit / crash drains.
        void open_stream(std::FILE* stream, io_poster poster, uint64_t max_queued) {
            close();
            attach(stream, false, std::move(poster), max_queued, false);
        }

        bool is_open() const { return state_ != nullptr; }
        bool async() const { return state_ && state_->poster; }
        // A write failed (after a close: as far as known when it closed).
        bool failed() const { return state_ ? state_->failed.load(std::memory_order_relaxed) : failed_; }
        // The most bytes ever waiting for the writer (this file).
        uint64_t peak_queued() const { return peak_; }
        uint64_t batches() const { return state_ ? state_->batches.load(std::memory_order_relaxed) : batches_; }

        // Hands `bytes` to the writer.  False: not open, or (writer thread
        // only) the queue holds max_queued bytes already and `force` is not
        // set - nothing was queued and nothing blocked.
        bool append(std::string bytes, bool force = false) {
            if (!state_ || bytes.empty()) return state_ != nullptr;
            auto& s = *state_;
            if (!s.poster) {
                std::lock_guard io(s.io_mutex);
                if (s.file && !s.failed.load(std::memory_order_relaxed)
                    && std::fwrite(bytes.data(), 1, bytes.size(), s.file) != bytes.size())
                    s.failed.store(true, std::memory_order_relaxed);
                return true;
            }
            bool post = false;
            {
                std::lock_guard q(s.queue_mutex);
                if (s.close_requested) return false;
                const uint64_t size = bytes.size();
                if (!force && s.max_queued != 0 && s.pending_bytes + size > s.max_queued) return false;
                s.pending.push_back(std::move(bytes));
                s.pending_bytes += size;
                if (s.pending_bytes > peak_) {
                    peak_ = s.pending_bytes;
                    s.peak_bytes.store(peak_, std::memory_order_relaxed);
                }
                if (!s.scheduled) s.scheduled = post = true;
            }
            if (post) s.poster([st = state_] { io_detail::run_batch(st); });
            return true;
        }

        // Explicit and blocking: everything queued is written and handed to
        // the OS.  Nothing in a session calls it per record (the user's
        // decision, step SIO: no proactive flush); the synchronous journal's
        // own per-record flushes (the legacy path, the SimTool) use it.
        void flush() {
            if (!state_) return;
            std::lock_guard io(state_->io_mutex);
            io_detail::drain_locked(*state_, true, false, io_detail::kWait);
        }

        // With a writer thread: posts the close and returns at once (the
        // strand writes what is queued, then closes).  Without one: closes
        // here.
        void close() {
            if (!state_) return;
            auto s = std::move(state_);
            batches_ = s->batches.load(std::memory_order_relaxed);
            failed_ = s->failed.load(std::memory_order_relaxed);
            if (s->poster) {
                bool post = false;
                {
                    std::lock_guard q(s->queue_mutex);
                    if (!s->close_requested) {
                        s->close_requested = true;
                        if (!s->scheduled) s->scheduled = post = true;
                    }
                }
                if (post) s->poster([s] { io_detail::run_batch(s); });
                closing_ = s;   // a reopen of the same path waits for it
                return;
            }
            std::lock_guard io(s->io_mutex);
            io_detail::finish_file(*s);
            failed_ = s->failed.load(std::memory_order_relaxed);
        }

        // Synchronous close on this thread: drain, close.  Waits for the
        // file at most `timeout` (a writer killed mid-write at process exit
        // must not hang the exit); on a timeout the file is left to the
        // strand / the exit drain.  Used by destructors and the capture.
        void close_now(std::chrono::milliseconds timeout) {
            if (!state_) return;
            auto s = std::move(state_);
            batches_ = s->batches.load(std::memory_order_relaxed);
            {
                std::lock_guard q(s->queue_mutex);
                s->close_requested = true;
            }
            if (!s->io_mutex.try_lock_for(timeout)) {
                if (s->poster) closing_ = s;   // left to the strand
                return;
            }
            io_detail::drain_locked(*s, false, true, timeout);
            io_detail::finish_file(*s);
            failed_ = s->failed.load(std::memory_order_relaxed);
            s->io_mutex.unlock();
        }

    private:
        void attach(std::FILE* f, bool own, io_poster poster, uint64_t max_queued, bool flush_batches) {
            auto s = std::make_shared<io_detail::queue_state>();
            s->file = f;
            s->own_file = own;
            s->flush_batches = flush_batches;
            s->poster = std::move(poster);
            s->max_queued = max_queued;
            peak_ = 0;
            batches_ = 0;
            failed_ = false;
            if (s->poster) io_detail::register_queue(s);
            state_ = std::move(s);
        }

        std::shared_ptr<io_detail::queue_state> state_;
        // The last file closed by a posted close (its strand may still have
        // it): what a reopen of the same path must wait for.
        std::weak_ptr<io_detail::queue_state> closing_;
        uint64_t peak_ = 0;     // the owner's copy (append runs on the owner's side)
        uint64_t batches_ = 0;   // after close
        bool failed_ = false;    // after close
    };

    namespace io {
        // Every queue with a writer thread is written out here and now: what
        // is queued is written, a requested close is done, the rest is handed
        // to the OS.  `timeout` per file (a crash may have killed the writer
        // holding it).  The process-exit path and the crash handlers
        // (best effort: the crash may be in the writer itself).
        inline void drain_all(std::chrono::milliseconds timeout) {
            if (!io_detail::registry_used().load(std::memory_order_acquire)) return;
            std::vector<std::shared_ptr<io_detail::queue_state>> queues;
            {
                auto& r = io_detail::registry();
                std::unique_lock lk(r.mutex, std::defer_lock);
                if (!lk.try_lock_for(timeout)) return;
                for (auto& w: r.queues)
                    if (auto s = w.lock()) queues.push_back(std::move(s));
            }
            for (auto& s: queues) {
                if (!s->io_mutex.try_lock_for(timeout)) continue;
                io_detail::drain_locked(*s, true, true, timeout);
                s->io_mutex.unlock();
            }
        }
    }

}
