/**
 * @file ir_text.h
 * @brief Textual serialization of the IFJ26 intermediate representation
 *
 * Defines a small line-based text format for IrProgram (see ir_text.c for the
 * full format description) together with a printer (IrProgram -> text) and a
 * parser (text -> IrProgram built through the public ir_helpers API).
 *
 * This is standalone playground tooling: it compiles against the repository's
 * public headers only and is not part of the compiler itself.
 */

#ifndef IR_TEXT_H
#define IR_TEXT_H

#include "ctx.h"  // Ctx

#include <stddef.h>  // size_t

/**
 * @brief Prints the IR program held in the context as text
 *
 * The output is a freshly allocated NUL-terminated string owned by the caller
 * and released with free().
 *
 * @param[in] ctx Non-null pointer to an initialized context containing the IR
 * @return The printed IR text, or NULL on allocation failure
 */
char* ir_text_print(Ctx* ctx);

/**
 * @brief Parses IR text into the context, replacing any IR held there
 *
 * The current content of ctx->ir is freed and the parsed program is built
 * through the ir_helpers constructors. Value and block ids may contain gaps in
 * the input (e.g. after optimization); they are remapped to sequential ids, so
 * printed output always re-parses to an equivalent program.
 *
 * Does not validate types, builtin call arities or SSA dominance - only the
 * structural integrity of the format.
 *
 * @param[in,out] ctx Non-null pointer to an initialized context
 * @param[in] text NUL-terminated IR text to parse
 * @param[out] error_buf Buffer for a "line N: ..." diagnostic on failure
 * @param[in] error_size Size of error_buf
 * @return true on success, false on parse error (error_buf is filled)
 */
bool ir_text_parse(Ctx* ctx, const char* text, char* error_buf, size_t error_size);

#endif
