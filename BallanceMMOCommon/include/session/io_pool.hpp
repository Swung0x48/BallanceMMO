#pragma once

// The asio half of session I/O (step SIO; session/io_queue.hpp is the
// asio-free half the journal includes).  A file writer posts its writes to an
// asio strand of the host's executor, so they run one at a time and in order
// without a dedicated std::thread per file:
//   - the retail mod passes its existing asio::thread_pool (BallanceMMOClient's
//     thread_pool_) to strand_poster;
//   - the server and the headless client own one io_pool (a one-thread
//     asio::thread_pool) for their session I/O.
// Included only where asio is on the include path (the server, the headless
// client, the retail mod, the tests).

#include "io_queue.hpp"

#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <cstddef>
#include <utility>

namespace bmmo::session {

    // One new strand of `executor`: every task posted through the returned
    // poster runs after the one posted before it.  One per file.
    template <class Executor>
    io_poster strand_poster(const Executor& executor) {
        auto strand = asio::make_strand(executor);
        return [strand](io_task task) { asio::post(strand, std::move(task)); };
    }

    class io_pool {
    public:
        explicit io_pool(std::size_t threads = 1) : pool_(threads) {}
        // Every queued write (the final closes included) runs before the
        // pool's threads go: join() without stop() waits for the work.  It
        // must outlive every queue that posts to it.
        ~io_pool() { pool_.join(); }
        io_pool(const io_pool&) = delete;
        io_pool& operator=(const io_pool&) = delete;

        io_poster strand() { return strand_poster(pool_.get_executor()); }

    private:
        asio::thread_pool pool_;
    };

}
