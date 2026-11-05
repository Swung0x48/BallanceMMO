#pragma once

// The coordinator of a canned test's run (docs/canned-tests-plan.md, step T1b):
// the network (a sim_network, one endpoint per process) and the time.  Every
// process connects over TCP (remote_transport.hpp); the run steps:
//   1. wait until every process is idle (it reported its earliest wake);
//   2. apply the ops the processes made since the last step, in the order
//      process, rank, sequence - one order whatever the threads' real timing;
//   3. the time: the earliest wake of all; the network advances to it;
//   4. every process gets what the network delivered to it by then and its
//      connection state changes (with the link's round trip once connected),
//      then the time.
// The run ends at until_us, or once every client (role 1) has gone; the server
// is then stopped.
// Processes are numbered by (role, name) once all have said hello, so the run
// does not depend on who connected first.  A process that disconnects is gone:
// its connections close (the peers learn it after their links' latency).
// Header-only; the run is one thread plus a reader per process.

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "coordinator_protocol.hpp"
#include "sim_transport.hpp"

namespace bmmo::net {

    struct coordinator_options {
        uint16_t port = 0;                    // 0: any free port (port() says which)
        size_t processes = 0;
        microseconds start_us = 3600ll * 1000 * 1000;   // virtual_clock::kDefaultStart
        microseconds until_us = 0;            // the run ends past it; 0: when no process waits for a time
        int client_latency_ms = 0;            // each way, on every client's end of its links
        float client_loss_pct = 0.0f;         // unreliable messages, each way, on every client's end
        uint64_t seed = 1;                    // the loss's
    };

    class coordinator {
    public:
        explicit coordinator(coordinator_options options)
            : options_(options), acceptor_(io_), network_(options.seed) {
            const asio::ip::tcp::endpoint at(asio::ip::make_address("127.0.0.1"), options.port);
            acceptor_.open(at.protocol());
            acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acceptor_.bind(at);
            acceptor_.listen();
        }
        ~coordinator() {
            for (auto& p: processes_) {
                std::error_code ec;
                p->socket.close(ec);
            }
            for (auto& p: processes_)
                if (p->reader.joinable()) p->reader.join();
        }

        uint16_t port() const { return acceptor_.local_endpoint().port(); }
        // Lines worth a log (a process that never goes idle).
        void set_report(std::function<void(const std::string&)> report) { report_ = std::move(report); }
        void set_trace(bool trace) { trace_ = trace; }
        uint64_t steps() const { return steps_; }
        microseconds now() const { return network_.now(); }

        bool run(std::string& error) {
            if (!welcome(error)) return false;
            network_.advance(options_.start_us - network_.now());
            for (;;) {
                if (!wait_all_idle()) break;   // every process gone
                // Every client gone (a client ends its own run): the run is over.
                if (std::none_of(processes_.begin(), processes_.end(), [](const auto& p) { return p->alive && p->role != 0; }))
                    break;
                apply_ops();
                std::optional<microseconds> next;
                for (auto& p: processes_)
                    if (p->alive && p->wake && (!next || *p->wake < *next)) next = p->wake;
                if (!next) { error = "no process waits for a time"; break; }
                if (options_.until_us && *next > options_.until_us) break;
                network_.advance(*next - network_.now());
                if (report_ && trace_) report_("step " + std::to_string(steps_) + " to +" + std::to_string(*next - options_.start_us) + " us");
                for (auto& p: processes_)
                    if (p->alive) hand_over(*p, *next);
                ++steps_;
            }
            // (The readers end when the processes close their ends, or with
            // the coordinator: never waited for here.)
            for (auto& p: processes_)
                if (p->alive) coord::write_frame(p->socket, coord::op::stop, {});
            return error.empty();
        }

    private:
        struct tagged_op {
            uint8_t rank = 0;
            uint64_t sequence = 0;
            coord::op o{};
            std::string body;   // after the tag
        };
        struct process {
            explicit process(asio::io_context& io) : socket(io) {}
            asio::ip::tcp::socket socket;
            uint8_t role = 0;
            std::string name;
            uint32_t index = 0;
            std::unique_ptr<transport> endpoint;
            std::map<connection, connection> to_sim, to_local;
            connection next_incoming = 0x10000;
            std::thread reader;
            // the run's thread only, fed by the reader through `inbox`
            bool alive = true, idle = false;
            std::optional<microseconds> wake;
            std::vector<tagged_op> ops;
            std::vector<status_change> refused;   // connects nothing listened for, told at the step's hand-over
        };
        struct frame_in {
            size_t process;
            bool eof = false;
            coord::op o{};
            std::string body;
        };

        bool welcome(std::string& error) {
            for (size_t i = 0; i < options_.processes; ++i) {
                auto p = std::make_unique<process>(io_);
                std::error_code ec;
                acceptor_.accept(p->socket, ec);
                if (ec) { error = "accept: " + ec.message(); return false; }
                p->socket.set_option(asio::ip::tcp::no_delay(true), ec);
                coord::op o{};
                std::string body;
                if (!coord::read_frame(p->socket, o, body) || o != coord::op::hello) { error = "a process said no hello"; return false; }
                coord::body_in in(body);
                p->role = in.get<uint8_t>();
                p->name = in.str();
                processes_.push_back(std::move(p));
            }
            std::sort(processes_.begin(), processes_.end(), [](const auto& a, const auto& b) {
                return a->role != b->role ? a->role < b->role : a->name < b->name;
            });
            for (uint32_t i = 0; i < processes_.size(); ++i) {
                auto& p = *processes_[i];
                p.index = i;
                p.endpoint = network_.make_transport();
                if (p.role != 0) {   // a client: the link's latency and loss on its end
                    for (const auto d: {transport::direction::send, transport::direction::recv}) {
                        p.endpoint->set_fake_lag(d, options_.client_latency_ms);
                        p.endpoint->set_fake_loss(d, options_.client_loss_pct);
                    }
                }
                coord::body_out w;
                w.put<uint32_t>(i);
                coord::write_frame(p.socket, coord::op::welcome, w.bytes());
                p.reader = std::thread([this, i] { read_loop(i); });
            }
            return true;
        }

        void read_loop(size_t i) {
            auto& socket = processes_[i]->socket;
            for (;;) {
                frame_in f{i};
                if (!coord::read_frame(socket, f.o, f.body)) f.eof = true;
                {
                    std::lock_guard lk(inbox_mutex_);
                    inbox_.push_back(std::move(f));
                }
                inbox_cv_.notify_one();
                if (f.eof) return;
            }
        }

        // Takes frames until every live process is idle; false when none is
        // left.
        bool wait_all_idle() {
            for (;;) {
                bool any = false, all = true;
                for (const auto& p: processes_)
                    if (p->alive) { any = true; all = all && p->idle; }
                if (!any) return false;
                if (all) return true;
                frame_in f;
                {
                    std::unique_lock lk(inbox_mutex_);
                    // A process that never goes idle stops the run: say which,
                    // once, after 5 s of real time.
                    if (!inbox_cv_.wait_for(lk, std::chrono::seconds(5), [&] { return !inbox_.empty(); })) {
                        if (!stalled_reported_ && report_) {
                            stalled_reported_ = true;
                            std::string who;
                            for (const auto& p: processes_)
                                if (p->alive && !p->idle) who += (who.empty() ? "" : ", ") + p->name;
                            report_("waiting 5 s of real time for " + who + " to go idle (step " + std::to_string(steps_) + ")");
                        }
                        continue;
                    }
                    f = std::move(inbox_.front());
                    inbox_.pop_front();
                }
                if (trace_ && report_)
                report_("frame " + std::to_string(static_cast<int>(f.o)) + (f.eof ? " eof" : "") + " from " + processes_[f.process]->name);
            take(*processes_[f.process], f);
            }
        }

        void take(process& p, const frame_in& f) {
            if (f.eof) {
                p.alive = false;
                for (const auto& [sim, local]: p.to_local) p.endpoint->close(sim, kEndApp, "process gone", false);
                return;
            }
            coord::body_in in(f.body);
            if (f.o == coord::op::idle) {
                const bool has = in.get<uint8_t>() != 0;
                const auto wake = in.get<int64_t>();
                p.wake = has ? std::optional<microseconds>(wake) : std::nullopt;
                p.idle = true;
                return;
            }
            tagged_op op;
            op.rank = in.get<uint8_t>();
            op.sequence = in.get<uint64_t>();
            op.o = f.o;
            op.body = f.body.substr(9);
            p.ops.push_back(std::move(op));
        }

        void apply_ops() {
            for (auto& pp: processes_) {
                auto& p = *pp;
                std::stable_sort(p.ops.begin(), p.ops.end(), [](const tagged_op& a, const tagged_op& b) {
                    return a.rank != b.rank ? a.rank < b.rank : a.sequence < b.sequence;
                });
                for (const auto& op: p.ops) apply(p, op);
                p.ops.clear();
            }
        }

        void apply(process& p, const tagged_op& op) {
            coord::body_in in(op.body);
            const auto sim_of = [&](connection local) {
                const auto it = p.to_sim.find(local);
                return it == p.to_sim.end() ? kInvalidConnection : it->second;
            };
            switch (op.o) {
            case coord::op::listen:
                p.endpoint->listen(in.get<uint16_t>());
                break;
            case coord::op::connect: {
                const connection local = in.get<uint32_t>();
                const connection sim = p.endpoint->connect(in.str());
                if (sim == kInvalidConnection) {
                    status_change c;
                    c.conn = local;
                    c.state = connection_state::problem_detected_locally;
                    c.end_debug = "nothing listens there";
                    p.refused.push_back(c);
                } else {
                    p.to_sim[local] = sim;
                    p.to_local[sim] = local;
                }
                break;
            }
            case coord::op::accept:
                p.endpoint->accept(sim_of(in.get<uint32_t>()));
                break;
            case coord::op::close: {
                const connection sim = sim_of(in.get<uint32_t>());
                const auto reason = in.get<int32_t>();
                const auto debug = in.str();
                p.endpoint->close(sim, reason, debug, in.get<uint8_t>() != 0);
                break;
            }
            case coord::op::send: {
                const connection sim = sim_of(in.get<uint32_t>());
                const auto flags = in.get<int32_t>();
                const auto bytes = in.str();
                p.endpoint->send(sim, bytes.data(), bytes.size(), flags);
                break;
            }
            case coord::op::set_name: {
                const connection sim = sim_of(in.get<uint32_t>());
                p.endpoint->set_name(sim, in.str());
                break;
            }
            case coord::op::set_lag: {
                const auto d = in.get<uint8_t>() == 0 ? transport::direction::send : transport::direction::recv;
                p.endpoint->set_fake_lag(d, in.get<int32_t>());
                break;
            }
            case coord::op::set_loss: {
                const auto d = in.get<uint8_t>() == 0 ? transport::direction::send : transport::direction::recv;
                p.endpoint->set_fake_loss(d, in.get<float>());
                break;
            }
            default:
                break;
            }
        }

        // What the network delivered to `p` by `now`, its state changes, the time.
        void hand_over(process& p, microseconds now) {
            const auto local_of = [&](connection sim) {
                const auto it = p.to_local.find(sim);
                if (it != p.to_local.end()) return it->second;
                const connection local = p.next_incoming++;   // a connection brought in
                p.to_local[sim] = local;
                p.to_sim[local] = sim;
                return local;
            };
            for (const auto& c: p.refused) send_status(p, c);
            p.refused.clear();
            p.endpoint->poll_status([&](const status_change& sc) {
                status_change c = sc;
                const connection sim = c.conn;
                c.conn = local_of(sim);
                send_status(p, c);
                connection_status st;
                if (c.state == connection_state::connected && p.endpoint->status(sim, st)) {
                    coord::body_out l;
                    l.put<uint32_t>(c.conn).put<int32_t>(st.ping_ms);
                    coord::write_frame(p.socket, coord::op::link, l.bytes());
                }
            });
            p.endpoint->receive([&](const message& m) {
                coord::body_out d;
                d.put<uint32_t>(local_of(m.conn)).put<int64_t>(m.received)
                    .str(std::string(static_cast<const char*>(m.data), m.size));
                coord::write_frame(p.socket, coord::op::deliver, d.bytes());
            }, 1 << 30);
            coord::body_out a;
            a.put<int64_t>(now);
            coord::write_frame(p.socket, coord::op::advance, a.bytes());
            p.idle = false;
        }

        void send_status(process& p, const status_change& c) {
            coord::body_out s;
            s.put<uint32_t>(c.conn).put<int32_t>(static_cast<int32_t>(c.state)).put<int32_t>(static_cast<int32_t>(c.old_state))
                .put<int32_t>(c.end_reason).str(c.end_debug).str(c.description);
            coord::write_frame(p.socket, coord::op::status, s.bytes());
        }

        coordinator_options options_;
        asio::io_context io_;
        asio::ip::tcp::acceptor acceptor_;
        sim_network network_;
        std::vector<std::unique_ptr<process>> processes_;
        std::mutex inbox_mutex_;
        std::condition_variable inbox_cv_;
        std::deque<frame_in> inbox_;
        uint64_t steps_ = 0;
        std::function<void(const std::string&)> report_;
        bool stalled_reported_ = false;
        bool trace_ = false;
    };

}
