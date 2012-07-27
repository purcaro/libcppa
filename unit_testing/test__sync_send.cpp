#include "test.hpp"
#include "cppa/cppa.hpp"

using namespace cppa;

/*

test case:

  A                  B                  C
  |                  |                  |
  | --(sync_send)--> |                  |
  |                  | --(forward)----> |
  |                  X                  |---\
  |                                     |   |
  |                                     |<--/
  | <-------------(reply)-------------- |

*/

struct A : event_based_actor {
    void init() {
        become (
            on(atom("go"), arg_match) >> [=](const actor_ptr& next) {
                handle_response(sync_send(next, atom("gogo"))) (
                    on(atom("gogogo")) >> [=] {
cout << "A received gogogo" << endl;
                        quit();
                    },
                    after(std::chrono::seconds(1)) >> [=] {
                        quit(exit_reason::user_defined);
                    }
                );
            },
            others() >> [=] {
cerr << "UNEXPECTED: " << to_string(last_dequeued()) << endl;
            }
        );
    }
};

struct B : event_based_actor {
    actor_ptr m_buddy;
    B(const actor_ptr& buddy) : m_buddy(buddy) { }
    void init() {
        become (
            others() >> [=]() {
cout << "B forward_to C" << endl;
                forward_to(m_buddy);
                quit();
            }
        );
    }
};

struct C : event_based_actor {
    void init() {
        become (
            on(atom("gogo")) >> [=] {
cout << "C received gogo (reply)" << endl;
                reply(atom("gogogo"));
                quit();
            }
        );
    }
};

int main() {
    send(spawn<A>(), atom("go"), spawn<B>(spawn<C>()));
    await_all_others_done();
    return 0;
}