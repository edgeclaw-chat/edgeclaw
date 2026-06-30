#ifndef UTLIST_FIX_H
#define UTLIST_FIX_H

#include "utlist.h"

/* Fix LL_APPEND2 double-evaluation of 'add' parameter.
 * Original evaluates (add) twice: once for next=NULL, once for append/head.
 * When (add) is a function call, each evaluation creates a new object. */
#undef LL_APPEND2
#define LL_APPEND2(head, add, next)                            \
    do {                                                       \
        LDECLTYPE(head) _tmp;                                  \
        LDECLTYPE(head) _add_ = (add);                         \
        _add_->next = NULL;                                    \
        if (head) {                                            \
            _tmp = (head);                                     \
            while (_tmp->next) { _tmp = _tmp->next; }          \
            _tmp->next = _add_;                                \
        } else {                                               \
            (head) = _add_;                                    \
        }                                                      \
    } while (0)

#endif /* UTLIST_FIX_H */
