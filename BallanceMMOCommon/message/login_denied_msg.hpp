#ifndef BALLANCEMMOSERVER_LOGIN_DENIED_MSG_HPP
#define BALLANCEMMOSERVER_LOGIN_DENIED_MSG_HPP

namespace bmmo {
    struct dummy_body {};
    typedef struct message<dummy_body, LoginDenied> login_denied_msg;
}

#endif //BALLANCEMMOSERVER_LOGIN_DENIED_MSG_HPP
