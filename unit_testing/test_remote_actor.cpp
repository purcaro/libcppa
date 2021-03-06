#include <thread>
#include <string>
#include <cstring>
#include <sstream>
#include <iostream>
#include <functional>

#include "test.hpp"
#include "ping_pong.hpp"
#include "cppa/cppa.hpp"
#include "cppa/logging.hpp"
#include "cppa/exception.hpp"

using namespace std;
using namespace cppa;

namespace {

typedef std::pair<std::string, std::string> string_pair;

typedef vector<actor_ptr> actor_vector;

vector<string_pair> get_kv_pairs(int argc, char** argv, int begin = 1) {
    vector<string_pair> result;
    for (int i = begin; i < argc; ++i) {
        auto vec = split(argv[i], '=');
        auto eq_fun = [&](const string_pair& p) { return p.first == vec[0]; };
        if (vec.size() != 2) {
            CPPA_PRINTERR("\"" << argv[i] << "\" is not a key-value pair");
        }
        else if (any_of(result.begin(), result.end(), eq_fun)) {
            CPPA_PRINTERR("key \"" << vec[0] << "\" is already defined");
        }
        else {
            result.emplace_back(vec[0], vec[1]);
        }
    }
    return result;
}

void reflector() {
    CPPA_SET_DEBUG_NAME("reflector" << self->id());
    become (
        others() >> [=] {
            CPPA_LOGF_INFO("reflect and quit");
            reply_tuple(self->last_dequeued());
            self->quit();
        }
    );
}

void spawn5_server_impl(actor_ptr client, group_ptr grp) {
    CPPA_LOGF_TRACE(CPPA_TARG(client, to_string)
                    << ", " << CPPA_TARG(grp, to_string));
    spawn_in_group(grp, reflector);
    spawn_in_group(grp, reflector);
    CPPA_LOGF_INFO("send {'Spawn5'} and await {'ok', actor_vector}");
    sync_send(client, atom("Spawn5"), grp).then(
        on(atom("ok"), arg_match) >> [=](const actor_vector& vec) {
            CPPA_LOGF_INFO("received vector with " << vec.size() << " elements");
            send(grp, "Hello reflectors!", 5.0);
            if (vec.size() != 5) {
                CPPA_PRINTERR("remote client did not spawn five reflectors!");
            }
            for (auto& a : vec) self->monitor(a);
        },
        others() >> [] {
            CPPA_UNEXPECTED_MSG();
            self->quit(exit_reason::unhandled_exception);
        },
        after(chrono::seconds(10)) >> [] {
            CPPA_UNEXPECTED_TOUT();
            self->quit(exit_reason::unhandled_exception);
        }
    )
    .continue_with([=] {
        CPPA_PRINT("wait for reflected messages");
        // receive seven reply messages (2 local, 5 remote)
        auto replies = std::make_shared<int>(0);
        become (
            on("Hello reflectors!", 5.0) >> [=] {
                if (++*replies == 7) {
                    CPPA_PRINT("wait for DOWN messages");
                    auto downs = std::make_shared<int>(0);
                    become (
                        on(atom("DOWN"), arg_match) >> [=](std::uint32_t reason) {
                            if (reason != exit_reason::normal) {
                                CPPA_PRINTERR("reflector exited for non-normal exit reason!");
                            }
                            if (++*downs == 5) {
                                CPPA_CHECKPOINT();
                                send(client, atom("Spawn5Done"));
                                self->quit();
                            }
                        },
                        others() >> [] {
                            CPPA_UNEXPECTED_MSG();
                            self->quit(exit_reason::unhandled_exception);
                        },
                        after(chrono::seconds(2)) >> [] {
                            CPPA_UNEXPECTED_TOUT();
                            self->quit(exit_reason::unhandled_exception);
                        }
                    );
                }
            },
            after(std::chrono::seconds(2)) >> [] {
                CPPA_UNEXPECTED_TOUT();
                self->quit(exit_reason::unhandled_exception);
            }
        );
    });
}

// receive seven reply messages (2 local, 5 remote)
void spawn5_server(actor_ptr client, bool inverted) {
    CPPA_SET_DEBUG_NAME("spawn5_server");
    if (!inverted) spawn5_server_impl(client, group::get("local", "foobar"));
    else {
        CPPA_LOGF_INFO("request group");
        sync_send(client, atom("GetGroup")).then (
            [=](const group_ptr& remote_group) {
                spawn5_server_impl(client, remote_group);
            }
        );
    }
}

void spawn5_client() {
    CPPA_SET_DEBUG_NAME("spawn5_client");
    become (
        on(atom("GetGroup")) >> [] {
            CPPA_LOGF_INFO("received {'GetGroup'}");
            reply(group::get("local", "foobar"));
        },
        on(atom("Spawn5"), arg_match) >> [=](const group_ptr& grp) {
            CPPA_LOGF_INFO("received {'Spawn5'}");
            actor_vector vec;
            for (int i = 0; i < 5; ++i) {
                vec.push_back(spawn_in_group(grp, reflector));
            }
            reply(atom("ok"), std::move(vec));
        },
        on(atom("Spawn5Done")) >> [] {
            CPPA_LOGF_INFO("received {'Spawn5Done'}");
            self->quit();
        }
    );
}

} // namespace <anonymous>

void verbose_terminate() {
    try { if (std::uncaught_exception()) throw; }
    catch (std::exception& e) {
        CPPA_PRINTERR("terminate called after throwing "
                      << to_verbose_string(e));
    }
    catch (...) {
        CPPA_PRINTERR("terminate called after throwing an unknown exception");
    }

    abort();
}

template<typename T>
void await_down(actor_ptr ptr, T continuation) {
    become (
        on(atom("DOWN"), arg_match) >> [=](uint32_t) -> bool {
            if (self->last_sender() == ptr) {
                continuation();
                return true;
            }
            return false; // not the 'DOWN' message we are waiting for
        }
    );
}

class client : public event_based_actor {

 public:

    client(actor_ptr server) : m_server(std::move(server)) { }

    void init() {
        CPPA_SET_DEBUG_NAME("client");
        spawn_ping();
    }

 private:

    void spawn_ping() {
        CPPA_PRINT("send {'SpawnPing'}");
        send(m_server, atom("SpawnPing"));
        become (
            on(atom("PingPtr"), arg_match) >> [=](const actor_ptr& ping) {
                auto pptr = spawn<monitored+detached+blocking_api>(pong, ping);
                await_down(pptr, [=] {
                    send_sync_msg();
                });
            }

        );
    }

    void send_sync_msg() {
        CPPA_PRINT("sync send {'SyncMsg'}");
        sync_send(m_server, atom("SyncMsg")).then(
            on(atom("SyncReply")) >> [=] {
                send_foobars();
            }
        );
    }

    void send_foobars(int i = 0) {
        if (i == 0) { CPPA_PRINT("send foobars"); }
        if (i == 100) test_group_comm();
        else {
            CPPA_LOG_DEBUG("send message nr. " << (i+1));
            sync_send(m_server, atom("foo"), atom("bar"), i).then (
                on(atom("foo"), atom("bar"), i) >> [=] {
                    send_foobars(i+1);
                }
            );
        }
    }

    void test_group_comm() {
        CPPA_PRINT("test group communication via network");
        sync_send(m_server, atom("GClient")).then(
            on(atom("GClient"), arg_match) >> [=](actor_ptr gclient) {
                auto s5a = spawn<monitored>(spawn5_server, gclient, false);
                await_down(s5a, [=]{
                    test_group_comm_inverted();
                });
            }
        );
    }

    void test_group_comm_inverted() {
        CPPA_PRINT("test group communication via network (inverted setup)");
        become (
            on(atom("GClient")) >> [=] {
                auto cptr = self->last_sender();
                auto s5c = spawn<monitored>(spawn5_client);
                reply(atom("GClient"), s5c);
                await_down(s5c, [=] {
                    CPPA_CHECKPOINT();
                    self->quit();
                });
            }
        );
    }

    actor_ptr m_server;

};

class server : public event_based_actor {

 public:

    void init() {
        CPPA_SET_DEBUG_NAME("server");
        await_spawn_ping();
    }

 private:

    void await_spawn_ping() {
        CPPA_PRINT("await {'SpawnPing'}");
        become (
            on(atom("SpawnPing")) >> [=] {
                CPPA_PRINT("received {'SpawnPing'}");
                auto client = self->last_sender();
                CPPA_LOGF_ERROR_IF(!client, "last_sender() == nullptr");
                CPPA_LOGF_INFO("spawn event-based ping actor");
                auto pptr = spawn<monitored>(event_based_ping, 10);
                reply(atom("PingPtr"), pptr);
                CPPA_LOGF_INFO("wait until spawned ping actor is done");
                await_down(pptr, [=] {
                    CPPA_CHECK_EQUAL(pongs(), 10);
                    await_sync_msg();
                });
            }
        );
    }

    void await_sync_msg() {
        CPPA_PRINT("await {'SyncMsg'}");
        become (
            on(atom("SyncMsg")) >> [=] {
                CPPA_PRINT("received {'SyncMsg'}");
                reply(atom("SyncReply"));
                await_foobars();
            }
        );
    }

    void await_foobars() {
        CPPA_PRINT("await foobars");
        auto foobars = make_shared<int>(0);
        become (
            on(atom("foo"), atom("bar"), arg_match) >> [=](int i) {
                ++*foobars;
                reply_tuple(self->last_dequeued());
                if (i == 99) {
                    CPPA_CHECK_EQUAL(*foobars, 100);
                    test_group_comm();
                }
            }
        );
    }

    void test_group_comm() {
        CPPA_PRINT("test group communication via network");
        become (
            on(atom("GClient")) >> [=] {
                auto cptr = self->last_sender();
                auto s5c = spawn<monitored>(spawn5_client);
                reply(atom("GClient"), s5c);
                await_down(s5c, [=] {
                    test_group_comm_inverted(cptr);
                });
            }
        );
    }

    void test_group_comm_inverted(actor_ptr cptr) {
        CPPA_PRINT("test group communication via network (inverted setup)");
        sync_send(cptr, atom("GClient")).then (
            on(atom("GClient"), arg_match) >> [=](actor_ptr gclient) {
                await_down(spawn<monitored>(spawn5_server, gclient, true), [=] {
                    CPPA_CHECKPOINT();
                    self->quit();
                });
            }
        );
    }

};

void run_client_part(const vector<string_pair>& args) {
    CPPA_LOGF_INFO("run in client mode");
    CPPA_TEST(test_remote_actor_client_part);
    auto i = find_if(args.begin(), args.end(),
                     [](const string_pair& p) { return p.first == "port"; });
    if (i == args.end()) {
        CPPA_LOGF_ERROR("no port specified");
        throw std::logic_error("no port specified");
    }
    auto port = static_cast<uint16_t>(stoi(i->second));
    auto serv = remote_actor("localhost", port);
    // remote_actor is supposed to return the same server when connecting to
    // the same host again
    {
        auto server2 = remote_actor("localhost", port);
        CPPA_CHECK(serv == server2);
    }
    auto c = spawn<client,monitored>(serv);
    receive (
        on(atom("DOWN"), arg_match) >> [=](uint32_t rsn) {
            CPPA_CHECK_EQUAL(self->last_sender(), c);
            CPPA_CHECK_EQUAL(rsn, exit_reason::normal);
        }
    );
}

int main(int argc, char** argv) {
    set_terminate(verbose_terminate);
    announce<actor_vector>();
    CPPA_SET_DEBUG_NAME("main");
    cout.unsetf(ios_base::unitbuf);
    string app_path = argv[0];
    bool run_remote_actor = true;
    if (argc > 1) {
        if (strcmp(argv[1], "run_remote_actor=false") == 0) {
            CPPA_LOGF_INFO("don't run remote actor");
            run_remote_actor = false;
        }
        else {
            run_client_part(get_kv_pairs(argc, argv));
            await_all_others_done();
            shutdown();
            return CPPA_TEST_RESULT();
        }
    }
    CPPA_TEST(test_remote_actor);
    auto serv = spawn<server,monitored>();
    uint16_t port = 4242;
    bool success = false;
    do {
        try {
            publish(serv, port, "127.0.0.1");
            success = true;
            CPPA_LOGF_DEBUG("running on port " << port);
        }
        catch (bind_failure&) {
            // try next port
            ++port;
        }
    }
    while (!success);
    thread child;
    ostringstream oss;
    if (run_remote_actor) {
        oss << app_path << " run=remote_actor port=" << port << " &>/dev/null";
        // execute client_part() in a separate process,
        // connected via localhost socket
        child = thread([&oss]() {
            CPPA_LOGC_TRACE("NONE", "main$thread_launcher", "");
            string cmdstr = oss.str();
            if (system(cmdstr.c_str()) != 0) {
                CPPA_PRINTERR("FATAL: command \"" << cmdstr << "\" failed!");
                abort();
            }
        });
    }
    else { CPPA_PRINT("actor published at port " << port); }
    CPPA_CHECKPOINT();
    receive (
        on(atom("DOWN"), arg_match) >> [=](uint32_t rsn) {
            CPPA_CHECK_EQUAL(self->last_sender(), serv);
            CPPA_CHECK_EQUAL(rsn, exit_reason::normal);
        }
    );
    // wait until separate process (in sep. thread) finished execution
    await_all_others_done();
    CPPA_CHECKPOINT();
    if (run_remote_actor) child.join();
    CPPA_CHECKPOINT();
    shutdown();
    return CPPA_TEST_RESULT();
}
