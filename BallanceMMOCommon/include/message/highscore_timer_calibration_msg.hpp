#ifndef BALLANCEMMOSERVER_HIGHSCORE_TIMER_CALIBRATION_MSG_HPP
#define BALLANCEMMOSERVER_HIGHSCORE_TIMER_CALIBRATION_MSG_HPP
#include "message.hpp"
#include "../entity/map.hpp"

namespace bmmo {
    struct highscore_timer_calibration {
        bmmo::map map{};
        int64_t time_diff_microseconds{};
    };

    typedef struct message<highscore_timer_calibration, HighscoreTimerCalibration> highscore_timer_calibration_msg;
}

#endif //BALLANCEMMOSERVER_HIGHSCORE_TIMER_CALIBRATION_MSG_HPP
