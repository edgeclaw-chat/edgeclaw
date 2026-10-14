#ifndef __BC_TOOLS_TODO_H__
#define __BC_TOOLS_TODO_H__

#include "sds.h"
#include <cJSON.h>

struct edc_toolbox;
extern void edc_tools_todo_register(struct edc_toolbox *tb);

/* Renders plain "- [ ]/- [x]" list rows (no <system-reminder> wrapper, no guidance). It
 * shares the same line-format source with render_reminder -- it is the todo reminder's
 * status body. *out_completed (may be NULL) returns the completed count. items NULL/empty
 * -> empty sds. Caller sdsfree. */

/* The full todo reminder text (two-part skeleton + <system-reminder> wrapper) -- the
 * SINGLE render source for the todowrite tool's result/echo and the LLM request-tail
 * reminder (edc_context_make_todo_reminder's content), byte-identical:
 *   <system-reminder>
 *   ## 🚨 CURRENT TASK SYSTEM STATE
 *   {list rows | No todo list exists for this task yet.}
 *
 *   ## 🛠️ INSTRUCTIONS FOR THIS TURN
 *   {per-turn cadence | planning guide}
 *   </system-reminder>
 * items non-empty -> status = list rows + cadence; items NULL/empty -> "no list yet" +
 * planning guide (the two branches are exclusive).
 * Returns sds, caller sdsfree. */
/* The two render functions (render_body / render_reminder) moved to edc_context.h: they
 * render the task's todos, which is context material. */

#endif
