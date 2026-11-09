#ifndef EDC_LIST_H
#define EDC_LIST_H

/*
 * Intrusive singly-linked list append, with the node type spelled out by the caller.
 *
 * Why this exists next to <utlist.h> (which the project also uses):
 *
 * utlist's append macros need a temporary of the node's type, which they obtain with
 * `__typeof` on GCC/Clang and `decltype` on MSVC **in C++ mode**. This project is C, so on
 * MSVC utlist falls back to its NO_DECLTYPE path, which uses the new node's own link field as
 * the cursor. That works, but it evaluates the `add` argument several times -- and the call
 * sites here pass constructor calls such as `edc_tool_param_new(...)`, so every append would
 * build several parameter objects and leak all but one. `utlist_fix.h` already had to
 * override LL_APPEND2 for exactly that reason, and its override is written with
 * `LDECLTYPE(head) _tmp; _tmp->next` -- valid on GCC, a syntax error under MSVC C, which is
 * why every tool definition failed to compile on Windows.
 *
 * The fix is to stop guessing the type: the caller names it. That removes the dependency on
 * typeof/decltype entirely, works the same on every compiler, and still evaluates `add`
 * exactly once, so the constructor-call call sites keep their single-allocation contract.
 *
 *   EDC_LL_APPEND(struct edc_tool_param, params, edc_tool_param_new(...));
 *   EDC_LL_APPEND2(struct edc_task, group->members, task, group_next);
 */

#define EDC_LL_APPEND2(type, head, add, field)                                                 \
    do                                                                                         \
    {                                                                                          \
        type *_edc_add_ = (add);                     /* evaluated exactly once */              \
        type *_edc_cursor_;                                                                    \
        _edc_add_->field = NULL;                                                               \
        if (head)                                                                              \
        {                                                                                      \
            _edc_cursor_ = (head);                                                             \
            while (_edc_cursor_->field)                                                        \
            {                                                                                  \
                _edc_cursor_ = _edc_cursor_->field;                                            \
            }                                                                                  \
            _edc_cursor_->field = _edc_add_;                                                   \
        }                                                                                      \
        else                                                                                   \
        {                                                                                      \
            (head) = _edc_add_;                                                                \
        }                                                                                      \
    } while (0)

/* The common case: the link field is named `next`. */
#define EDC_LL_APPEND(type, head, add) EDC_LL_APPEND2(type, head, add, next)

#endif /* EDC_LIST_H */
