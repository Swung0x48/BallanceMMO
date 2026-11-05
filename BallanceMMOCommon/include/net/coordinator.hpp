#pragma once

// The coordinator of a canned test's run (docs/canned-tests-plan.md, steps
// T1b, T1e): the network (a sim_network, one endpoint per process) and the
// time.  Every process connects over TCP (remote_transport.hpp); the run goes
// in rounds:
//   1. wait until every process is idle (it reported its earliest wake past
//      the last grant) or gone;
//   2. apply the ops the processes made in the round, in the order time,
//      process, rank, sequence - one order whatever the threads' real timing;
//   3. the round's start t0: the earliest wake of all; its end, the grant:
//      t0 + the network's lowest latency - 1 us (nothing sent from t0 on can
//      arrive by then, so every process may run to it on its own), or t0 with
//      `lockstep` (one wake time per round - the reference the lookahead must
//      equal) or no latency;
//   4. every process gets what reaches it by the grant - messages and state
//      changes, each with its time - then the grant.
// A message or a change that turns up later inside a lookahead round would have
// reached a process that may already have run past it: the run fails, loudly.
// The run ends at until_us, or kWindDown past the last client's going (plus the
// link's latency) - one time in every mode; the server is then stopped.
// Processes are numbered by (role, name) once all have said hello, so the run
// does not depend on who connected first.  A process that goes (bye, or its end
// of the TCP connection) closes its connections then (the peers learn it after
// their links' latency).
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
#include <tuple>
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
        bool lockstep = false;                // one wake time per round (no lookahead)
    };

    class coordinator {
    public:
        // After the last client goes, the server runs this much longer (past
        // the link's latency): its session sees the members go.
        static constexpr microseconds kWindDown = 1000 * 1000;
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
        uint64_t rounds() const { return rounds_; }
        microseconds now() const { return network_.now(); }

        bool run(std::string& error) {
            if (!welcome(error)) return false;
            network_.advance_to(options_.start_us);
            std::optional<microseconds> end_at = options_.until_us ? std::optional<microseconds>(options_.until_us) : std::nullopt;
            bool clients_gone = false;
            for (;;) {
                if (!wait_all_idle()) break;   // every process gone
                apply_ops();
                // Every client gone (a client ends its own run): the rest
                // runs to one time whatever the rounds - the last client's
                // going, the link's latency (its close reaches the server)
                // and kWindDown - then the run is over.
                if (!clients_gone && std::none_of(processes_.begin(), processes_.end(),
                                                  [](const auto& p) { return p->alive && p->role != 0; })) {
                    clients_gone = true;
                    microseconds last = network_.now();
                    for (const auto& p: processes_)
                        if (p->role != 0) last = std::max(last, p->bye.value_or(p->last_at));
                    const microseconds latency = network_.lowest_latency();
                    const microseconds at = last + (latency == sim_network::kNoLink ? 0 : latency) + kWindDown;
                    end_at = end_at ? std::min(*end_at, at) : at;
                }
                std::optional<microseconds> next;
                for (auto& p: processes_)
                    if (p->alive && p->wake && (!next || *p->wake < *next)) next = p->wake;
                if (!next) { if (!clients_gone) error = "no process waits for a time"; break; }
                if (end_at && *next > *end_at) break;
                const microseconds latency = options_.lockstep ? 0 : network_.lowest_latency();
                microseconds until = latency > 0 ? *next + latency - 1 : *next;
                if (end_at) until = std::min(until, *end_at);
                network_.advance_to(*next);
                if (report_ && trace_)
                    report_("round " + std::to_string(rounds_) + ": +" + std::to_string(*next - options_.start_us) + " to +"
                            + std::to_string(until - options_.start_us) + " us");
                for (auto& p: processes_)
                    if (p->alive && !hand_over(*p, until, error)) break;
                if (!error.empty()) break;
                lookahead_ = until > *next;
                stall_reports_ = 0;
                granted_ = until;
                ++rounds_;
            }
            // (The readers end when the processes close their ends, or with
            // the coordinator: never waited for here.)
            for (auto& p: processes_)
                if (p->alive) coord::write_frame(p->socket, coord::op::stop, {});
            return error.empty();
        }

    private:
        struct tagged_op {
            microseconds at = 0;
            uint8_t rank = 0;
            uint64_t sequence = 0;
            coord::op o{};
            std::string body;   // after the tag
        };
        struct refusal {   // a connect nothing listened for, told after the link's round trip
            microseconds at = 0;
            connection local = kInvalidConnection;
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
            bool going = false;                   // gone, its connections not yet closed
            std::optional<microseconds> bye;      // when it said it goes
            microseconds last_at = 0;             // its latest op's time
            std::optional<microseconds> wake;
            std::vector<tagged_op> ops;
            std::vector<refusal> refused;
            int lag_ms[2] = {0, 0};               // its end's latency: send, recv
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
                        p.lag_ms[d == transport::direction::send ? 0 : 1] = options_.client_latency_ms;
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
            coord::frame_reader reader(processes_[i]->socket);
            for (;;) {
                frame_in f{i};
                if (!reader.next(f.o, f.body)) f.eof = true;
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
                    // A process that does not go idle stops the run: say which,
                    // every 5 s of real time (at most 12 times a round).
                    if (!inbox_cv_.wait_for(lk, std::chrono::seconds(5), [&] { return !inbox_.empty(); })) {
                        if (report_ && stall_reports_ < 12) {
                            ++stall_reports_;
                            std::string who, rest;
                            for (const auto& p: processes_) {
                                if (p->alive && !p->idle) who += (who.empty() ? "" : ", ") + p->name;
                                if (p->alive && p->idle && p->wake)
                                    rest += " " + p->name + "@+" + std::to_string(*p->wake - options_.start_us);
                            }
                            report_("waiting 5 s of real time for " + who + " to go idle (round " + std::to_string(rounds_)
                                    + ", granted to +" + std::to_string(granted_ - options_.start_us) + " us; idle:" + rest + ")");
                        }
                        continue;
                    }
                    f = std::move(inbox_.front());
                    inbox_.pop_front();
                }
                take(*processes_[f.process], f);
            }
        }

        void take(process& p, const frame_in& f) {
            if (f.eof) {
                p.alive = false;
                p.going = true;
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
            if (f.o == coord::op::bye) {
                p.bye = in.get<int64_t>();
                return;
            }
            tagged_op op;
            op.rank = in.get<uint8_t>();
            op.sequence = in.get<uint64_t>();
            op.at = in.get<int64_t>();
            op.o = f.o;
            op.body = f.body.substr(coord::kTagSize);
            p.last_at = std::max(p.last_at, op.at);
            p.ops.push_back(std::move(op));
        }

        // The round's ops, in time order (then process, rank, sequence); a
        // process gone closes its connections at its bye (or its last op).
        void apply_ops() {
            struct ordered {
                microseconds at;
                uint32_t process;
                uint8_t rank;
                uint64_t sequence;
                const tagged_op* op;   // null: the process goes
            };
            std::vector<ordered> all;
            for (auto& pp: processes_) {
                auto& p = *pp;
                for (const auto& op: p.ops) all.push_back({op.at, p.index, op.rank, op.sequence, &op});
                if (p.going)
                    all.push_back({std::max(p.bye.value_or(p.last_at), network_.now()), p.index, uint8_t(255), UINT64_MAX, nullptr});
            }
            std::sort(all.begin(), all.end(), [](const ordered& a, const ordered& b) {
                return std::tie(a.at, a.process, a.rank, a.sequence) < std::tie(b.at, b.process, b.rank, b.sequence);
            });
            for (const auto& o: all) {
                network_.advance_to(o.at);
                auto& p = *processes_[o.process];
                if (o.op) {
                    apply(p, *o.op);
                } else {
                    for (const auto& [sim, local]: p.to_local) p.endpoint->close(sim, kEndApp, "process gone", false);
                    p.going = false;
                }
            }
            for (auto& p: processes_) p->ops.clear();
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
                    // told after a round trip on this end's link (at least 1 us)
                    const microseconds rtt = static_cast<microseconds>(p.lag_ms[0] + p.lag_ms[1]) * 1000;
                    p.refused.push_back({network_.now() + std::max<microseconds>(rtt, 1), local});
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
                const auto ms = in.get<int32_t>();
                p.lag_ms[d == transport::direction::send ? 0 : 1] = ms;
                p.endpoint->set_fake_lag(d, ms);
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

        // What reaches `p` by `until`, then the grant.  Anything due inside
        // the last lookahead round turned up too late: an error.
        bool hand_over(process& p, microseconds until, std::string& error) {
            const auto local_of = [&](connection sim) {
                const auto it = p.to_local.find(sim);
                if (it != p.to_local.end()) return it->second;
                const connection local = p.next_incoming++;   // a connection brought in
                p.to_local[sim] = local;
                p.to_sim[local] = sim;
                return local;
            };
            const auto check = [&](microseconds at, const char* what) {
                if (lookahead_ && at <= granted_ && error.empty())
                    error = std::string(what) + " to " + p.name + " due at +" + std::to_string(at - options_.start_us)
                          + " us, inside the round granted to +" + std::to_string(granted_ - options_.start_us) + " us";
            };
            std::string out;
            for (auto it = p.refused.begin(); it != p.refused.end();) {
                if (it->at > until) { ++it; continue; }
                check(it->at, "a refusal");
                coord::body_out s;
                s.put<uint32_t>(it->local).put<int64_t>(it->at)
                    .put<int32_t>(static_cast<int32_t>(connection_state::problem_detected_locally))
                    .put<int32_t>(kEndInvalid).str("nothing listens there").put<int32_t>(-1);
                coord::append_frame(out, coord::op::status, s.bytes());
                it = p.refused.erase(it);
            }
            network_.take(*p.endpoint, until,
                [&](connection sim, microseconds at, const std::string& bytes) {
                    check(at, "a message");
                    coord::body_out d;
                    d.put<uint32_t>(local_of(sim)).put<int64_t>(at).str(bytes);
                    coord::append_frame(out, coord::op::deliver, d.bytes());
                },
                [&](connection sim, microseconds at, connection_state state, int reason, const std::string& debug) {
                    check(at, "a state change");
                    connection_status st;
                    const bool linked = (state == connection_state::connecting || state == connection_state::connected)
                                        && p.endpoint->status(sim, st);
                    coord::body_out s;
                    s.put<uint32_t>(local_of(sim)).put<int64_t>(at).put<int32_t>(static_cast<int32_t>(state))
                        .put<int32_t>(reason).str(debug).put<int32_t>(linked ? st.ping_ms : -1);
                    coord::append_frame(out, coord::op::status, s.bytes());
                });
            coord::body_out g;
            g.put<int64_t>(until);
            coord::append_frame(out, coord::op::grant, g.bytes());
            coord::write_all(p.socket, out);
            p.idle = false;
            return error.empty();
        }

        coordinator_options options_;
        asio::io_context io_;
        asio::ip::tcp::acceptor acceptor_;
        sim_network network_;
        std::vector<std::unique_ptr<process>> processes_;
        std::mutex inbox_mutex_;
        std::condition_variable inbox_cv_;
        std::deque<frame_in> inbox_;
        uint64_t rounds_ = 0;
        bool lookahead_ = false;      // the last round ran past its start
        microseconds granted_ = 0;    // the last round's grant
        std::function<void(const std::string&)> report_;
        int stall_reports_ = 0;   // this round's
        bool trace_ = false;
    };

}
