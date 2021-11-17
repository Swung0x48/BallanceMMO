#ifndef BALLANCEMMOSERVER_ENTITY_HPP
#define BALLANCEMMOSERVER_ENTITY_HPP

namespace bmmo {
    struct vec3 {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct quaternion {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double w = 0.0;
    };

    struct ball_state {
        vec3 position;
        quaternion quaternion;
    };
}

#endif //BALLANCEMMOSERVER_ENTITY_HPP
