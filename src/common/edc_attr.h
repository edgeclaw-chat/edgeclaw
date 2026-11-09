#ifndef EDC_ATTR_H
#define EDC_ATTR_H

/*
 * Compile-time attributes that are worth having on GCC/Clang and have no MSVC spelling.
 *
 * This exists because the attribute is not decoration: `format(printf, ...)` is what makes
 * every call site of a printf-like function format-checked, and without it a `%s` handed an
 * int64_t compiles silently. MSVC has no equivalent, so on Windows the check is simply lost
 * -- that is a deliberate trade, not an oversight. What is NOT acceptable is writing the GCC
 * attribute unconditionally, which is what these three declarations used to do: cl.exe stops
 * with `error C2061: syntax error: identifier '__attribute__'` on the declaration itself, so
 * the error surfaced in every file that included the header, far from the actual cause.
 *
 * Deliberately kept apart from edc_compat.h: that header pulls in <windows.h>, and this one
 * is included from headers (edc_error.h, edc_string_utils.h) that every layer depends on.
 */

#if defined(__GNUC__) || defined(__clang__)
#define EDC_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define EDC_NORETURN                 __attribute__((noreturn))
#define EDC_UNUSED                   __attribute__((unused))
#else
#define EDC_PRINTF(fmt_idx, arg_idx)
#define EDC_NORETURN
#define EDC_UNUSED
#endif

#endif /* EDC_ATTR_H */
