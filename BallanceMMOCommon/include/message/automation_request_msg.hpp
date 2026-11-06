#ifndef BALLANCEMMOSERVER_AUTOMATION_REQUEST_MSG_HPP
#define BALLANCEMMOSERVER_AUTOMATION_REQUEST_MSG_HPP
#include "message.hpp"
#include "action_denied_msg.hpp"
#include <cstdint>

namespace bmmo {
    // A client automation verb that changes the game outside its rules (beam, sector, key, ...)
    // runs only when the server approves it: the client asks with the request's id and the verb,
    // the server answers with an AutomationVerdict (approved for an operator; otherwise denied with
    // the reason, and its console logs the attempt).
    struct automation_request {
        uint32_t id = 0;
        char verb[16] = {};   // NUL-terminated, for the server's log
    };

    struct automation_verdict {
        uint32_t id = 0;
        uint8_t approved = 0;
        deny_reason reason = deny_reason::Unknown;
        uint8_t reserved[2] = {};
    };

    typedef struct message<automation_request, AutomationRequest> automation_request_msg;
    typedef struct message<automation_verdict, AutomationVerdict> automation_verdict_msg;
    static_assert(sizeof(automation_request_msg) == 24 && sizeof(automation_verdict_msg) == 12, "the wire layouts");
}

#endif //BALLANCEMMOSERVER_AUTOMATION_REQUEST_MSG_HPP
