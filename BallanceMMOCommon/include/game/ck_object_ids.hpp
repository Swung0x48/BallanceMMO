#pragma once

// Every object id of a CK context at one moment: asked by id (count) and
// walked in ascending order, as the std::set<CK_ID> it replaces was.  A level
// context holds ~100k objects; that set cost ~8 ms to build and each scan
// that looked every id up in it as much again.  A trafo set's build (every
// member's join) made three of them: ~39 of a join's 47 ms on a P-core, the
// whole world step stalled for it (docs/realtime-design.md, 'Measured (the
// cost of a join)').  A bitmap indexed by id costs a pass over CK's object
// list.

#include <cstddef>
#include <vector>

#include "CKAll.h"

namespace bmmo::game {
    class ck_object_ids {
    public:
        explicit ck_object_ids(CKContext* context) {
            // A reference into CK's own list: on the retail client the SDK's
            // XArray buffers belong to CK2.dll's heap, never copied here.
            const XObjectPointerArray& all = context->GetObjectListByType(CKCID_OBJECT, TRUE);
            for (int i = 0; i < all.Size(); ++i) {
                CKObject* o = all[i];
                if (!o) continue;
                const size_t id = static_cast<size_t>(o->GetID());
                if (id >= present_.size()) present_.resize(id + 1, false);
                present_[id] = true;
            }
            for (size_t id = 0; id < present_.size(); ++id)
                if (present_[id]) ids_.push_back(static_cast<CK_ID>(id));
        }
        // The std::set spelling, so a caller reads as it did.
        size_t count(CK_ID id) const {
            const size_t i = static_cast<size_t>(id);
            return i < present_.size() && present_[i] ? 1 : 0;
        }
        std::vector<CK_ID>::const_iterator begin() const { return ids_.begin(); }
        std::vector<CK_ID>::const_iterator end() const { return ids_.end(); }
        size_t size() const { return ids_.size(); }

    private:
        std::vector<bool> present_;
        std::vector<CK_ID> ids_;   // ascending
    };

    inline ck_object_ids all_object_ids(CKContext* context) { return ck_object_ids(context); }
}
