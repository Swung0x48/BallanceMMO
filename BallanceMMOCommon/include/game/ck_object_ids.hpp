#pragma once

// Every object id of a CK context at one moment, and the ids the context has
// gained since (added, ascending): what a copy made.
// A level context holds ~100k objects.  The std::set<CK_ID> this replaces
// cost ~8 ms to build and each scan that looked every id up in it as much
// again, and a trafo set's build (every member's join) made three: ~39 of a
// join's 47 ms on a P-core, the whole world step stalled for it
// (docs/realtime-design.md, 'Measured (the cost of a join)').
//
// The ids come from CK's per-class id lists (GetObjectsListByClassID, every
// class under CKCID_OBJECT), which is what GetObjectListByType(CKCID_OBJECT,
// derived) is built from: that call made an array of every object's pointer,
// and reading each object for its id touched ~100k objects - 2.5-3.8 ms a
// snapshot on the public server's machine.  The lists are read where they
// lie; the snapshot is a bitmap of them, and `added` one pass over them
// against it.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "CKAll.h"

namespace bmmo::game {
    // Every live object's id, class by class (CK's own lists, read where they
    // lie): the objects CK's by-name lookups walk (GetObjectByNameAndParentClass
    // with CKCID_OBJECT: every class, each list in order, a dead id skipped).
    template <class F>
    void for_each_object_id(CKContext* context, F&& f) {
        const int classes = CKGetClassCount();
        for (CK_CLASSID cid = 0; cid < classes; ++cid) {
            if (!CKIsChildClassOf(cid, CKCID_OBJECT)) continue;
            // CK's own buffer (on the retail client CK2.dll's heap):
            // read, never kept.
            const CK_ID* list = context->GetObjectsListByClassID(cid);
            const int n = list ? context->GetObjectsCountByClassID(cid) : 0;
            for (int i = 0; i < n; ++i)
                if (context->GetObject(list[i])) f(list[i]);   // as the object list left a dead id out
        }
    }

    class ck_object_ids {
    public:
        explicit ck_object_ids(CKContext* context) {
            each(context, [this](CK_ID id) {
                const size_t i = static_cast<size_t>(id);
                if (i >= present_.size()) present_.resize(i + 1, false);
                present_[i] = true;
            });
        }
        // The ids `context` holds now and did not hold at the snapshot,
        // ascending and each once (the order the set's walk gave them in).
        std::vector<CK_ID> added(CKContext* context) const {
            std::vector<CK_ID> ids;
            each(context, [&](CK_ID id) { if (!held(id)) ids.push_back(id); });
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            return ids;
        }

    private:
        bool held(CK_ID id) const {
            const size_t i = static_cast<size_t>(id);
            return i < present_.size() && present_[i];
        }
        template <class F>
        static void each(CKContext* context, F&& f) { for_each_object_id(context, std::forward<F>(f)); }
        std::vector<bool> present_;
    };
}
