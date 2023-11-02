#ifndef BALLANCEMMOSERVER_ENTITY_HPP
#define BALLANCEMMOSERVER_ENTITY_HPP

namespace bmmo {
    union vec3 {
        struct {
            float x, y, z;
        };
        float v[3] = {};
    };

    union quaternion {
        struct {
            float x, y, z, w;
        };
        float v[4] = {};
    };
}

#endif //BALLANCEMMOSERVER_ENTITY_HPP
