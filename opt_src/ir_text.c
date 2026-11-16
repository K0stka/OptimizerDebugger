/**
 * @file ir_text.c
 * @brief Textual serialization of the IFJ26 intermediate representation
 *
 * The format is line-based; '#' starts a comment running to the end of the
 * line (except inside string literals). Empty lines are ignored.
 *
 * @code
 * .entrypoint main                 # program entrypoint function (optional,
 *                                  # defaults to the first function)
 *
 * .func <name> : <return-type>
 * .param @<name> : <type>          # parameters must precede local slots
 * .local @<name> : <type>
 * .entry <block-id>                # function entrypoint (defaults to 0)
 *
 * .block <id>                      # block ids are sequential from 0
 *   v<id> : <type> = add v0, int 2 # value ids may have gaps
 *   store @i v0
 *   branch v0 ? 1 : 2              # exactly one terminator per block, last
 * @endcode
 *
 * Types: @c int | @c double | @c string | @c bool | @c unit
 *
 * Operands: @c v<id> | @c int <i32> | @c double <x> | @c string "..." |
 * @c bool true|false | @c unit
 *
 * Instructions (form depends on class):
 *  - binary:  @c v<id> : <type> = add|sub|mul|idiv|div|eq|ne|lt|le|gt|ge|concat <op>, <op>
 *  - unary:   @c v<id> : <type> = not|int2double|floor <op>
 *  - load:    @c v<id> : <type> = load @<slot>
 *  - store:   @c store @<slot> <op>              (never produces a value)
 *  - call:    @c [v<id> : <type> =] call <fn>[, <op>]...  (bare form for unit results)
 *
 * Terminators: @c jump <block> | @c branch <op> ? <block> : <block> |
 * @c ret <op> | @c exit <op>
 *
 * Function names in @c call resolve to built-ins first (print, readLine,
 * readInt, readBoolean, readDouble, toInt, toStr, strLen, strSub, ord, chr),
 * then to user functions defined by @c .func. Slot names must be declared
 * before use. Value ids may contain gaps (e.g. after optimization); they are
 * preserved on parse, so printed output keeps the supplied identifiers.
 */

#include "ir_text.h"

#include "ir_helpers.h"  // ir_add_* constructors, ir_get_instruction_class

#include <ctype.h>    // isalpha, isalnum, isdigit, isxdigit, tolower
#include <stdarg.h>   // va_list, va_start, va_end
#include <stdbool.h>  // bool
#include <stdint.h>   // SIZE_MAX, INT32_MIN, INT32_MAX
#include <stdio.h>    // vsnprintf, snprintf
#include <stdlib.h>   // malloc, realloc, free, strtol, strtod
#include <string.h>   // strlen, memcpy, strncmp, strcmp

////////////////////////////////////////////////////////////////////////////////
// Dynamic text buffer
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Growable character buffer used to assemble text output
 */
typedef struct {
  char* data;      // NUL-terminated contents (NULL while empty)
  size_t len;      // Length of the contents
  size_t capacity; // Allocated size of data
} TextBuf;

/**
 * @brief Appends count bytes to the buffer
 *
 * @param[in,out] buf Buffer to append to
 * @param[in] data Bytes to append
 * @param[in] count Number of bytes to append
 * @return false on allocation failure
 */
static bool buf_append(TextBuf* buf, const char* data, size_t count) {
  if (buf->len + count + 1 > buf->capacity) {
    size_t capacity = buf->capacity == 0 ? 256 : buf->capacity;
    while (buf->len + count + 1 > capacity)
      capacity *= 2;

    char* grown = realloc(buf->data, capacity);
    if (grown == NULL)
      return false;
    buf->data = grown;
    buf->capacity = capacity;
  }

  memcpy(buf->data + buf->len, data, count);
  buf->len += count;
  buf->data[buf->len] = '\0';
  return true;
}

/**
 * @brief Appends a NUL-terminated string to the buffer
 */
static bool buf_puts(TextBuf* buf, const char* str) {
  return buf_append(buf, str, strlen(str));
}

/**
 * @brief Appends formatted data to the buffer
 */
static bool buf_printf(TextBuf* buf, const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list args_copy;
  va_copy(args_copy, args);
  int needed = vsnprintf(NULL, 0, format, args);
  va_end(args);

  if (needed < 0) {
    va_end(args_copy);
    return false;
  }

  char stack_buffer[128];
  char* heap_buffer = NULL;
  char* target = stack_buffer;
  if ((size_t)needed + 1 > sizeof(stack_buffer)) {
    heap_buffer = malloc((size_t)needed + 1);
    if (heap_buffer == NULL) {
      va_end(args_copy);
      return false;
    }
    target = heap_buffer;
  }

  vsnprintf(target, (size_t)needed + 1, format, args_copy);
  va_end(args_copy);

  bool result = buf_append(buf, target, (size_t)needed);
  free(heap_buffer);
  return result;
}

////////////////////////////////////////////////////////////////////////////////
// Shared helpers
////////////////////////////////////////////////////////////////////////////////

/// Hard limit on value/block ids to catch nonsensical input early
#define MAX_TOKEN_ID 100000000ull

/**
 * @brief Returns the textual name of an IR type
 */
static const char* ir_type_name(IrType type) {
  switch (type) {
    case IR_INT:
      return "int";
    case IR_DOUBLE:
      return "double";
    case IR_STRING:
      return "string";
    case IR_BOOL:
      return "bool";
    case IR_UNIT:
      return "unit";
    default:
      return "?";
  }
}

/**
 * @brief Parses a type name into an IrType value
 *
 * @param[in] word Start of the word
 * @param[in] len Length of the word
 * @param[out] out Parsed type
 * @return false if the word is not a type name
 */
static bool ir_type_parse(const char* word, size_t len, IrType* out) {
  if (len == 3 && strncmp(word, "int", len) == 0)
    *out = IR_INT;
  else if (len == 6 && strncmp(word, "double", len) == 0)
    *out = IR_DOUBLE;
  else if (len == 6 && strncmp(word, "string", len) == 0)
    *out = IR_STRING;
  else if (len == 4 && strncmp(word, "bool", len) == 0)
    *out = IR_BOOL;
  else if (len == 4 && strncmp(word, "unit", len) == 0)
    *out = IR_UNIT;
  else
    return false;
  return true;
}

/**
 * @brief Names of the built-in functions, indexed by (IrFunctionType - IR_FX_PRINT)
 */
static const char* const BUILTIN_NAMES[] = {
    "print",   "readLine", "readInt",  "readBool", "readDouble", "toInt",
    "toStr",   "strLen",   "strSub",   "ord",      "chr",
};

////////////////////////////////////////////////////////////////////////////////
// Printer
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Appends a string operand as a quoted, escaped literal
 */
static bool print_escaped_string(TextBuf* buf, const char* str) {
  if (!buf_puts(buf, "\""))
    return false;

  for (const unsigned char* c = (const unsigned char*)(str == NULL ? "" : str);
       *c != '\0'; c++) {
    bool ok = true;
    switch (*c) {
      case '\\':
        ok = buf_puts(buf, "\\\\");
        break;
      case '"':
        ok = buf_puts(buf, "\\\"");
        break;
      case '\n':
        ok = buf_puts(buf, "\\n");
        break;
      case '\t':
        ok = buf_puts(buf, "\\t");
        break;
      case '\r':
        ok = buf_puts(buf, "\\r");
        break;
      default:
        // Escape other control characters as hex
        if (*c < 0x20 || *c == 0x7f)
          ok = buf_printf(buf, "\\x%02x", *c);
        else
          ok = buf_append(buf, (const char*)c, 1);
        break;
    }
    if (!ok)
      return false;
  }

  return buf_puts(buf, "\"");
}

/**
 * @brief Prints a double in a form that round-trips through strtod
 */
static bool print_double(TextBuf* buf, double value) {
  char text[40];
  for (int precision = 15; precision <= 17; precision++) {
    snprintf(text, sizeof(text), "%.*g", precision, value);
    if (strtod(text, NULL) == value)
      break;
  }
  return buf_puts(buf, text);
}

/**
 * @brief Appends a single operand to the buffer
 */
static bool print_operand(TextBuf* buf, const IrOperand* operand) {
  switch (operand->type) {
    case IR_OP_VALUE:
      return buf_printf(buf, "v%zu", operand->value.id);
    case IR_OP_INT32:
      return buf_printf(buf, "int %d", operand->value.int32_value);
    case IR_OP_DOUBLE:
      if (!buf_puts(buf, "double "))
        return false;
      return print_double(buf, operand->value.double_value);
    case IR_OP_STRING:
      if (!buf_puts(buf, "string "))
        return false;
      return print_escaped_string(buf, operand->value.string_value);
    case IR_OP_BOOL:
      return buf_puts(buf, operand->value.bool_value ? "bool true" : "bool false");
    case IR_OP_UNIT:
      return buf_puts(buf, "unit");
    default:
      return buf_puts(buf, "?invalid");
  }
}

/**
 * @brief Returns the name of a stack slot, or a fallback if unnamed
 */
static const char* slot_name(const IrFunction* fx, size_t slot) {
  if (slot < fx->slots.count && fx->slots.data[slot].name != NULL)
    return fx->slots.data[slot].name;
  return "?slot";
}

/**
 * @brief Returns the name of the function referenced by a call instruction
 */
static const char* call_function_name(Ctx* ctx, const IrInstruction* inst) {
  if (inst->operands.call.type == IR_FX_USER) {
    IrFunctionId id = inst->operands.call.id;
    if (id < ctx->ir.functions.count)
      return ctx->ir.functions.data[id].name;
    return "?function";
  }

  size_t index = (size_t)inst->operands.call.type - (size_t)IR_FX_PRINT;
  if (index < sizeof(BUILTIN_NAMES) / sizeof(BUILTIN_NAMES[0]))
    return BUILTIN_NAMES[index];
  return "?builtin";
}

/**
 * @brief Appends a single instruction to the buffer
 */
static bool print_instruction(TextBuf* buf, Ctx* ctx, IrFunction* fx,
                              IrInstruction* inst) {
  if (!buf_puts(buf, "  "))
    return false;

  // Instructions producing a value are prefixed with the value id and type
  if (inst->result != IR_INVALID_ID) {
    if (!buf_printf(buf, "v%zu : %s = ", inst->result,
                    ir_type_name(inst->result_type)))
      return false;
  }

  switch (ir_get_instruction_class(inst)) {
    case IR_INST_CLASS_BINARY:
      switch (inst->type) {
        case IR_INST_ADD: if (!buf_puts(buf, "add ")) return false; break;
        case IR_INST_SUB: if (!buf_puts(buf, "sub ")) return false; break;
        case IR_INST_MUL: if (!buf_puts(buf, "mul ")) return false; break;
        case IR_INST_IDIV: if (!buf_puts(buf, "idiv ")) return false; break;
        case IR_INST_DIV: if (!buf_puts(buf, "div ")) return false; break;
        case IR_INST_EQ: if (!buf_puts(buf, "eq ")) return false; break;
        case IR_INST_NE: if (!buf_puts(buf, "ne ")) return false; break;
        case IR_INST_LT: if (!buf_puts(buf, "lt ")) return false; break;
        case IR_INST_LE: if (!buf_puts(buf, "le ")) return false; break;
        case IR_INST_GT: if (!buf_puts(buf, "gt ")) return false; break;
        case IR_INST_GE: if (!buf_puts(buf, "ge ")) return false; break;
        case IR_INST_CONCAT: if (!buf_puts(buf, "concat ")) return false; break;
        default: return false;
      }
      if (!print_operand(buf, &inst->operands.binary.left))
        return false;
      if (!buf_puts(buf, ", "))
        return false;
      return print_operand(buf, &inst->operands.binary.right);

    case IR_INST_CLASS_UNARY:
      switch (inst->type) {
        case IR_INST_NOT: if (!buf_puts(buf, "not ")) return false; break;
        case IR_INST_INT2DOUBLE: if (!buf_puts(buf, "int2double ")) return false; break;
        case IR_INST_FLOOR: if (!buf_puts(buf, "floor ")) return false; break;
        default: return false;
      }
      return print_operand(buf, &inst->operands.unary.operand);

    case IR_INST_CLASS_LOAD:
      return buf_printf(buf, "load @%s",
                        slot_name(fx, inst->operands.load.source));

    case IR_INST_CLASS_STORE:
      if (!buf_printf(buf, "store @%s ",
                      slot_name(fx, inst->operands.store.target)))
        return false;
      return print_operand(buf, &inst->operands.store.source);

    case IR_INST_CLASS_CALL:
      if (!buf_printf(buf, "call %s", call_function_name(ctx, inst)))
        return false;
      for (size_t i = 0; i < inst->operands.call.arg_count; i++) {
        if (!buf_puts(buf, ", "))
          return false;
        if (!print_operand(buf, &inst->operands.call.args[i]))
          return false;
      }
      return true;
  }
  return false;
}

/**
 * @brief Appends a block terminator to the buffer
 */
static bool print_terminator(TextBuf* buf, IrBlockTerminator* terminator) {
  if (!buf_puts(buf, "  "))
    return false;

  switch (terminator->type) {
    case IR_TERMINATOR_JUMP:
      return buf_printf(buf, "jump %zu", terminator->value.jump_target);

    case IR_TERMINATOR_CONDITIONAL:
      if (!buf_puts(buf, "branch "))
        return false;
      if (!print_operand(buf, &terminator->value.conditional_target.condition))
        return false;
      return buf_printf(buf, " ? %zu : %zu",
                        terminator->value.conditional_target.true_target,
                        terminator->value.conditional_target.false_target);

    case IR_TERMINATOR_RETURN:
      if (!buf_puts(buf, "ret "))
        return false;
      return print_operand(buf, &terminator->value.return_value);

    case IR_TERMINATOR_EXIT:
      if (!buf_puts(buf, "exit "))
        return false;
      return print_operand(buf, &terminator->value.exit_code);

    default:
      return buf_puts(buf, "?invalid terminator");
  }
}

char* ir_text_print(Ctx* ctx) {
  TextBuf buf = {0};

  // Program entrypoint
  if (ctx->ir.functions.count > 0) {
    IrFunctionId entrypoint = ctx->ir.entrypoint;
    if (entrypoint >= ctx->ir.functions.count)
      entrypoint = 0;
    const char* name = ctx->ir.functions.data[entrypoint].name;
    if (!buf_printf(&buf, ".entrypoint %s\n\n", name == NULL ? "?function" : name))
      goto fail;
  }

  for (size_t f = 0; f < ctx->ir.functions.count; f++) {
    IrFunction* fx = &ctx->ir.functions.data[f];

    if (!buf_printf(&buf, ".func %s : %s\n",
                    fx->name == NULL ? "?function" : fx->name,
                    ir_type_name(fx->return_type)))
      goto fail;

    for (size_t s = 0; s < fx->slots.count; s++) {
      const char* kind = s < fx->parameter_count ? ".param" : ".local";
      if (!buf_printf(&buf, "%s @%s : %s\n", kind, slot_name(fx, s),
                      ir_type_name(fx->slots.data[s].type)))
        goto fail;
    }

    size_t entry = fx->entrypoint == IR_INVALID_ID ? 0 : fx->entrypoint;
    if (!buf_printf(&buf, ".entry %zu\n\n", entry))
      goto fail;

    for (size_t b = 0; b < fx->blocks.count; b++) {
      IrBlock* block = &fx->blocks.data[b];

      if (!buf_printf(&buf, ".block %zu\n", b))
        goto fail;

      for (size_t i = 0; i < block->instructions.count; i++) {
        if (!print_instruction(&buf, ctx, fx, &block->instructions.data[i]))
          goto fail;
        if (!buf_puts(&buf, "\n"))
          goto fail;
      }

      if (!print_terminator(&buf, &block->terminator))
        goto fail;
      if (!buf_puts(&buf, "\n\n"))
        goto fail;
    }
  }

  return buf.data == NULL ? calloc(1, 1) : buf.data;

fail:
  free(buf.data);
  return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Parser
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Parse error state threaded through the parser
 */
typedef struct {
  Ctx* ctx;             // Context being filled
  char* error;          // Error message buffer (may be NULL)
  size_t error_size;
  bool failed;          // Whether a parse error was already reported
  int current_line;     // 1-based line number for prefixes (0 = unknown)
  // Function registry filled by the first pass (names point into the line
  // buffer, which stays alive for the whole parse)
  const char** fnames_names;
  IrFunctionId* fnames_ids;
  size_t function_count;
  size_t fnames_capacity;
} Parser;

/**
 * @brief One meaningful (non-empty, comment-stripped) source line
 */
typedef struct {
  char* text;  // Pointer into the mutable text buffer
  int no;      // 1-based line number in the original text
} Line;

/**
 * @brief Records a parse error (first error wins)
 */
static void parse_fail(Parser* parser, const char* format, ...) {
  if (parser->failed)
    return;
  parser->failed = true;

  if (parser->error == NULL || parser->error_size == 0)
    return;

  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  if (parser->current_line > 0)
    snprintf(parser->error, parser->error_size, "line %d: %s",
             parser->current_line, message);
  else
    snprintf(parser->error, parser->error_size, "%s", message);
}

/**
 * @brief Character scanner over a single line
 */
typedef struct {
  const char* p;
} Scan;

/**
 * @brief Skips whitespace
 */
static void scan_ws(Scan* scan) {
  while (*scan->p == ' ' || *scan->p == '\t')
    scan->p++;
}

/**
 * @brief Compares a length-bounded word with a literal
 */
static bool word_is(const char* word, size_t len, const char* literal) {
  return len == strlen(literal) && strncmp(word, literal, len) == 0;
}

/**
 * @brief Scans an identifier-like word [A-Za-z_][A-Za-z0-9_]*
 */
static bool scan_word(Scan* scan, const char** start, size_t* len) {
  scan_ws(scan);
  if (!(isalpha((unsigned char)*scan->p) || *scan->p == '_'))
    return false;

  *start = scan->p;
  while (isalnum((unsigned char)*scan->p) || *scan->p == '_')
    scan->p++;
  *len = (size_t)(scan->p - *start);
  return true;
}

/**
 * @brief Scans a single literal character (after whitespace)
 */
static bool scan_char(Scan* scan, char c) {
  scan_ws(scan);
  if (*scan->p != c)
    return false;
  scan->p++;
  return true;
}

/**
 * @brief Scans a decimal id (value or block), rejecting oversized input
 */
static bool scan_id(Scan* scan, size_t* out) {
  scan_ws(scan);
  if (!isdigit((unsigned char)*scan->p))
    return false;

  size_t value = 0;
  while (isdigit((unsigned char)*scan->p)) {
    value = value * 10 + (size_t)(*scan->p - '0');
    if (value > MAX_TOKEN_ID)
      return false;
    scan->p++;
  }
  *out = value;
  return true;
}

/**
 * @brief Verifies the scanner reached the end of the line
 */
static bool scan_end(Parser* parser, Scan* scan) {
  scan_ws(scan);
  if (*scan->p != '\0') {
    parse_fail(parser, "unexpected characters at end of line");
    return false;
  }
  return true;
}

/**
 * @brief Decodes a quoted string literal with escapes
 *
 * Supported escapes: \\ \" \n \t \r \xHH. The result is a freshly allocated
 * NUL-terminated string owned by the caller.
 */
static bool scan_string(Parser* parser, Scan* scan, char** out) {
  scan_ws(scan);
  if (*scan->p != '"') {
    parse_fail(parser, "expected string literal");
    return false;
  }
  scan->p++;

  size_t capacity = 16, length = 0;
  char* data = malloc(capacity);
  if (data == NULL) {
    parse_fail(parser, "out of memory");
    return false;
  }

  bool ok = true;
  while (ok) {
    if (*scan->p == '"') {
      scan->p++;
      break;
    }
    if (*scan->p == '\0') {
      parse_fail(parser, "unterminated string literal");
      ok = false;
      break;
    }

    char decoded = 0;
    if (*scan->p == '\\') {
      scan->p++;
      switch (*scan->p) {
        case '\\':
        case '"':
          decoded = *scan->p;
          break;
        case 'n':
          decoded = '\n';
          break;
        case 't':
          decoded = '\t';
          break;
        case 'r':
          decoded = '\r';
          break;
        case 'x': {
          int value = 0;
          for (int i = 0; i < 2; i++) {
            int digit = -1;
            if (isdigit((unsigned char)scan->p[1]))
              digit = scan->p[1] - '0';
            else if (isxdigit((unsigned char)scan->p[1]))
              digit = tolower((unsigned char)scan->p[1]) - 'a' + 10;
            if (digit < 0) {
              parse_fail(parser, "\\x escape needs two hex digits");
              ok = false;
              break;
            }
            value = value * 16 + digit;
            scan->p++;
          }
          if (ok && value == 0)
            parse_fail(parser, "\\x00 escape is not supported");
          decoded = (char)value;
          break;
        }
        default:
          parse_fail(parser, "unknown escape '\\%c'", *scan->p);
          ok = false;
          break;
      }
      if (!ok)
        break;
      scan->p++;
    } else {
      decoded = *scan->p;
      scan->p++;
    }

    if (length + 1 >= capacity) {
      capacity *= 2;
      char* grown = realloc(data, capacity);
      if (grown == NULL) {
        parse_fail(parser, "out of memory");
        ok = false;
        break;
      }
      data = grown;
    }
    data[length++] = decoded;
  }

  if (!ok) {
    free(data);
    return false;
  }

  data[length] = '\0';
  *out = data;
  return true;
}

/**
 * @brief Frees a heap-allocated string operand (the IR keeps its own copy)
 */
static void free_operand(IrOperand* operand) {
  if (operand->type == IR_OP_STRING && operand->value.string_value != NULL)
    free(operand->value.string_value);
}

/**
 * @brief Parses one operand; value references are stored unresolved
 */
static bool parse_operand(Parser* parser, Scan* scan, IrOperand* out) {
  scan_ws(scan);

  // Value reference
  if (scan->p[0] == 'v' && isdigit((unsigned char)scan->p[1])) {
    scan->p++;
    size_t token;
    if (!scan_id(scan, &token)) {
      parse_fail(parser, "invalid value id");
      return false;
    }
    out->type = IR_OP_VALUE;
    out->value.id = token;
    return true;
  }

  // Typed constant
  const char* word;
  size_t len;
  if (!scan_word(scan, &word, &len)) {
    parse_fail(parser, "expected an operand");
    return false;
  }

  if (word_is(word, len, "int")) {
    scan_ws(scan);
    char* end = NULL;
    long value = strtol(scan->p, &end, 10);
    if (end == scan->p ||
        (*end != '\0' && *end != ' ' && *end != '\t' && *end != ',')) {
      parse_fail(parser, "expected an integer literal after 'int'");
      return false;
    }
    if (value < INT32_MIN || value > INT32_MAX) {
      parse_fail(parser, "integer literal out of the 32-bit range: %ld", value);
      return false;
    }
    scan->p = end;
    out->type = IR_OP_INT32;
    out->value.int32_value = (IrInt)value;
    return true;
  }

  if (word_is(word, len, "double")) {
    scan_ws(scan);
    char* end = NULL;
    double value = strtod(scan->p, &end);
    if (end == scan->p ||
        (*end != '\0' && *end != ' ' && *end != '\t' && *end != ',')) {
      parse_fail(parser, "expected a number after 'double'");
      return false;
    }
    scan->p = end;
    out->type = IR_OP_DOUBLE;
    out->value.double_value = value;
    return true;
  }

  if (word_is(word, len, "bool")) {
    const char* flag;
    size_t flag_len;
    if (!scan_word(scan, &flag, &flag_len) ||
        (!word_is(flag, flag_len, "true") && !word_is(flag, flag_len, "false"))) {
      parse_fail(parser, "expected 'true' or 'false' after 'bool'");
      return false;
    }
    out->type = IR_OP_BOOL;
    out->value.bool_value = word_is(flag, flag_len, "true");
    return true;
  }

  if (word_is(word, len, "string")) {
    char* decoded = NULL;
    if (!scan_string(parser, scan, &decoded))
      return false;
    out->type = IR_OP_STRING;
    out->value.string_value = decoded;
    return true;
  }

  if (word_is(word, len, "unit")) {
    out->type = IR_OP_UNIT;
    return true;
  }

  parse_fail(parser, "unknown operand '%.*s'", (int)(len > 31 ? 31 : len), word);
  return false;
}

/**
 * @brief Parses an IrType word
 */
static bool parse_type(Parser* parser, Scan* scan, IrType* out) {
  const char* word;
  size_t len;
  if (!scan_word(scan, &word, &len) || !ir_type_parse(word, len, out)) {
    parse_fail(parser, "expected a type (int, double, string, bool, unit)");
    return false;
  }
  return true;
}

/**
 * @brief Parses a function name
 */
static bool parse_name(Parser* parser, Scan* scan, const char** name, size_t* len) {
  if (!scan_word(scan, name, len)) {
    parse_fail(parser, "expected a name");
    return false;
  }
  return true;
}

/**
 * @brief Parses a free function header: name : return-type
 */
static bool parse_func_header(Parser* parser, Scan* scan, const char** name,
                              size_t* name_len, IrType* return_type) {
  if (!parse_name(parser, scan, name, name_len))
    return false;

  if (!scan_char(scan, ':')) {
    parse_fail(parser, "expected ':' after the function name");
    return false;
  }

  if (!parse_type(parser, scan, return_type))
    return false;

  return scan_end(parser, scan);
}

/**
 * @brief Maps a binary opcode word to an instruction type
 */
static bool parse_binary_opcode(const char* word, size_t len, IrInstructionType* out) {
  if (word_is(word, len, "add")) *out = IR_INST_ADD;
  else if (word_is(word, len, "sub")) *out = IR_INST_SUB;
  else if (word_is(word, len, "mul")) *out = IR_INST_MUL;
  else if (word_is(word, len, "idiv")) *out = IR_INST_IDIV;
  else if (word_is(word, len, "div")) *out = IR_INST_DIV;
  else if (word_is(word, len, "eq")) *out = IR_INST_EQ;
  else if (word_is(word, len, "ne")) *out = IR_INST_NE;
  else if (word_is(word, len, "lt")) *out = IR_INST_LT;
  else if (word_is(word, len, "le")) *out = IR_INST_LE;
  else if (word_is(word, len, "gt")) *out = IR_INST_GT;
  else if (word_is(word, len, "ge")) *out = IR_INST_GE;
  else if (word_is(word, len, "concat")) *out = IR_INST_CONCAT;
  else return false;
  return true;
}

/**
 * @brief Maps a unary opcode word to an instruction type
 */
static bool parse_unary_opcode(const char* word, size_t len, IrInstructionType* out) {
  if (word_is(word, len, "not")) *out = IR_INST_NOT;
  else if (word_is(word, len, "int2double")) *out = IR_INST_INT2DOUBLE;
  else if (word_is(word, len, "floor")) *out = IR_INST_FLOOR;
  else return false;
  return true;
}

/**
 * @brief Resolves a function name to a builtin type or user function id
 */
static bool resolve_function(Parser* parser, const char* name, size_t len,
                             IrFunctionType* type_out, IrFunctionId* id_out) {
  // Builtins first
  size_t builtin_count = sizeof(BUILTIN_NAMES) / sizeof(BUILTIN_NAMES[0]);
  for (size_t i = 0; i < builtin_count; i++) {
    if (word_is(name, len, BUILTIN_NAMES[i])) {
      *type_out = (IrFunctionType)((size_t)IR_FX_PRINT + i);
      *id_out = IR_INVALID_ID;
      return true;
    }
  }

  // Then user functions (registered during the first pass)
  for (size_t i = 0; i < parser->function_count; i++) {
    if (word_is(name, len, parser->fnames_names[i])) {
      *type_out = IR_FX_USER;
      *id_out = parser->fnames_ids[i];
      return true;
    }
  }

  parse_fail(parser, "call to unknown function '%.*s'",
             (int)(len > 63 ? 63 : len), name);
  return false;
}

/**
 * @brief Per-function parse state
 */
typedef struct {
  IrFunctionId fid;         // Id of the function being parsed
  bool active;              // Whether a .func section is open
  size_t block_count;       // Blocks created so far
  size_t* vmap;             // Raw value token -> assigned id (SIZE_MAX = unset)
  size_t vmap_capacity;
  bool* terminators;        // Whether each block already has its terminator
  size_t terminators_capacity;
  const char** slot_names;  // Slot names in declaration order (for lookups)
  size_t slot_count;
  size_t slot_capacity;
  bool saw_local;           // A .local was seen (params must precede locals)
  bool has_entry;
  size_t entry;
} FnState;

/**
 * @brief Preserves a defined value token and records it for reference validation
 */
static bool vmap_assign(Parser* parser, FnState* fx, size_t token, size_t mapped) {
  if (token > MAX_TOKEN_ID) {
    parse_fail(parser, "value id v%zu is too large", token);
    return false;
  }

  if (token + 1 > fx->vmap_capacity) {
    size_t capacity = fx->vmap_capacity == 0 ? 16 : fx->vmap_capacity;
    while (token + 1 > capacity)
      capacity *= 2;

    size_t* grown = realloc(fx->vmap, capacity * sizeof(size_t));
    if (grown == NULL) {
      parse_fail(parser, "out of memory");
      return false;
    }
    for (size_t i = fx->vmap_capacity; i < capacity; i++)
      grown[i] = SIZE_MAX;
    fx->vmap = grown;
    fx->vmap_capacity = capacity;
  }

  if (fx->vmap[token] != SIZE_MAX) {
    parse_fail(parser, "value v%zu is defined twice", token);
    return false;
  }

  IrFunction* function = &parser->ctx->ir.functions.data[fx->fid];
  IrBlock* block = &function->blocks.data[fx->block_count - 1];
  block->instructions.data[block->instructions.count - 1].result = token;
  // Undo the constructor's automatic id allocation, then reserve the textual id.
  function->value_count = mapped > token ? mapped : token + 1;
  fx->vmap[token] = token;
  return true;
}

/**
 * @brief Looks up a slot id by name (must be declared before use)
 */
static bool slot_lookup(Parser* parser, FnState* fx, const char* name, size_t len,
                        size_t* out) {
  for (size_t i = 0; i < fx->slot_count; i++) {
    if (word_is(name, len, fx->slot_names[i])) {
      *out = i;
      return true;
    }
  }

  parse_fail(parser, "reference to undeclared slot '@%.*s'",
             (int)(len > 63 ? 63 : len), name);
  return false;
}

/**
 * @brief Remaps a value reference to its assigned id
 */
static bool remap_operand(Parser* parser, FnState* fx, IrOperand* operand,
                          const char* fx_name) {
  if (operand->type != IR_OP_VALUE)
    return true;

  if (operand->value.id >= fx->vmap_capacity ||
      fx->vmap[operand->value.id] == SIZE_MAX) {
    parse_fail(parser, "function '%s': reference to undefined value v%zu",
               fx_name == NULL ? "?" : fx_name, operand->value.id);
    return false;
  }

  operand->value.id = fx->vmap[operand->value.id];
  return true;
}

/**
 * @brief Validates and finalizes a function after its lines were parsed
 *
 * Validates value references, block targets and the
 * entrypoint, and releases the per-function state. Reports errors without line
 * numbers (the line context is already gone); call with current_line = 0.
 */
static bool finish_function(Parser* parser, FnState* fx) {
  bool ok = true;

  if (fx->active) {
    IrFunction* function = &parser->ctx->ir.functions.data[fx->fid];
    const char* fx_name = function->name == NULL ? "?" : function->name;

    if (fx->block_count == 0) {
      parse_fail(parser, "function '%s' has no blocks", fx_name);
      ok = false;
    }

    // Every block needs a terminator
    for (size_t b = 0; ok && b < fx->block_count; b++) {
      if (!fx->terminators[b]) {
        parse_fail(parser, "function '%s': block %zu has no terminator", fx_name, b);
        ok = false;
      }
    }

    // Instructions: remap value references
    for (size_t b = 0; ok && b < fx->block_count; b++) {
      IrBlock* block = &function->blocks.data[b];

      for (size_t i = 0; ok && i < block->instructions.count; i++) {
        IrInstruction* inst = &block->instructions.data[i];
        switch (ir_get_instruction_class(inst)) {
          case IR_INST_CLASS_UNARY:
            ok = remap_operand(parser, fx, &inst->operands.unary.operand, fx_name);
            break;
          case IR_INST_CLASS_BINARY:
            ok = remap_operand(parser, fx, &inst->operands.binary.left, fx_name);
            if (ok)
              ok = remap_operand(parser, fx, &inst->operands.binary.right, fx_name);
            break;
          case IR_INST_CLASS_STORE:
            ok = remap_operand(parser, fx, &inst->operands.store.source, fx_name);
            break;
          case IR_INST_CLASS_CALL:
            for (size_t a = 0; ok && a < inst->operands.call.arg_count; a++)
              ok = remap_operand(parser, fx, &inst->operands.call.args[a], fx_name);
            break;
          case IR_INST_CLASS_LOAD:
            break;
        }
      }

      // Terminator: remap operands and validate block targets
      IrBlockTerminator* term = &block->terminator;
      if (ok) {
        switch (term->type) {
          case IR_TERMINATOR_JUMP:
            if (term->value.jump_target >= fx->block_count) {
              parse_fail(parser,
                         "function '%s': block %zu jumps to undefined block %zu",
                         fx_name, b, term->value.jump_target);
              ok = false;
            }
            break;
          case IR_TERMINATOR_CONDITIONAL:
            ok = remap_operand(parser, fx,
                               &term->value.conditional_target.condition, fx_name);
            if (ok &&
                (term->value.conditional_target.true_target >= fx->block_count ||
                 term->value.conditional_target.false_target >= fx->block_count)) {
              parse_fail(parser,
                         "function '%s': branch in block %zu targets an undefined block",
                         fx_name, b);
              ok = false;
            }
            break;
          case IR_TERMINATOR_RETURN:
            ok = remap_operand(parser, fx, &term->value.return_value, fx_name);
            break;
          case IR_TERMINATOR_EXIT:
            ok = remap_operand(parser, fx, &term->value.exit_code, fx_name);
            break;
        }
      }
    }

    // Entrypoint
    if (ok) {
      if (!fx->has_entry)
        function->entrypoint = 0;
      else if (fx->entry < fx->block_count)
        function->entrypoint = fx->entry;
      else {
        parse_fail(parser, "function '%s': entrypoint block %zu does not exist",
                   fx_name, fx->entry);
        ok = false;
      }
    }
  }

  free(fx->vmap);
  fx->vmap = NULL;
  fx->vmap_capacity = 0;
  free(fx->terminators);
  fx->terminators = NULL;
  fx->terminators_capacity = 0;
  free(fx->slot_names);
  fx->slot_names = NULL;
  fx->slot_count = 0;
  fx->slot_capacity = 0;
  fx->active = false;

  return ok;
}

/**
 * @brief Registers a new block in the current function
 */
static bool parse_block_directive(Parser* parser, FnState* fx, Scan* scan) {
  size_t id;
  if (!scan_id(scan, &id)) {
    parse_fail(parser, "expected a block id after '.block'");
    return false;
  }
  scan_ws(scan);
  if (*scan->p == ':')  // tolerate a trailing colon
    scan->p++;
  if (!scan_end(parser, scan))
    return false;

  if (id != fx->block_count) {
    parse_fail(parser, "block ids must be sequential: expected .block %zu",
               fx->block_count);
    return false;
  }

  ir_add_block(parser->ctx, fx->fid);
  fx->block_count++;

  if (fx->block_count > fx->terminators_capacity) {
    size_t capacity = fx->terminators_capacity == 0 ? 8 : fx->terminators_capacity * 2;
    bool* grown = realloc(fx->terminators, capacity * sizeof(bool));
    if (grown == NULL) {
      parse_fail(parser, "out of memory");
      return false;
    }
    fx->terminators = grown;
    fx->terminators_capacity = capacity;
  }
  fx->terminators[fx->block_count - 1] = false;
  return true;
}

/**
 * @brief Registers the current slot name for later lookups
 */
static bool slot_register(Parser* parser, FnState* fx, char* name) {
  if (fx->slot_count + 1 > fx->slot_capacity) {
    size_t capacity = fx->slot_capacity == 0 ? 8 : fx->slot_capacity * 2;
    const char** grown = realloc(fx->slot_names, capacity * sizeof(char*));
    if (grown == NULL) {
      parse_fail(parser, "out of memory");
      return false;
    }
    fx->slot_names = grown;
    fx->slot_capacity = capacity;
  }
  fx->slot_names[fx->slot_count++] = name;
  return true;
}

/**
 * @brief Parses '.param' / '.local' slot declarations
 */
static bool parse_slot_directive(Parser* parser, FnState* fx, Scan* scan, bool param) {
  if (!fx->active) {
    parse_fail(parser, "'%s' outside of a function", param ? ".param" : ".local");
    return false;
  }
  if (param && fx->saw_local) {
    parse_fail(parser, "parameters must be declared before local slots");
    return false;
  }

  scan_ws(scan);
  if (*scan->p != '@') {
    parse_fail(parser, "expected '@name' in slot declaration");
    return false;
  }
  scan->p++;
  const char* name;
  size_t name_len;
  if (!parse_name(parser, scan, &name, &name_len))
    return false;

  if (!scan_char(scan, ':')) {
    parse_fail(parser, "expected ':' after the slot name");
    return false;
  }
  IrType type;
  if (!parse_type(parser, scan, &type))
    return false;
  if (!scan_end(parser, scan))
    return false;

  // NUL-terminate the name in the line buffer (the line is fully parsed now)
  char* mutable_name = (char*)name;
  mutable_name[name_len] = '\0';

  // Duplicate names would make slot lookups ambiguous
  for (size_t i = 0; i < fx->slot_count; i++) {
    if (word_is(mutable_name, name_len, fx->slot_names[i])) {
      parse_fail(parser, "duplicate slot name '@%s'", mutable_name);
      return false;
    }
  }

  if (param)
    ir_add_parameter(parser->ctx, fx->fid, mutable_name, type);
  else
    ir_add_slot(parser->ctx, fx->fid, mutable_name, type);

  if (!slot_register(parser, fx, mutable_name))
    return false;

  if (!param)
    fx->saw_local = true;
  return true;
}

/**
 * @brief Parses and adds one instruction line to the current block
 */
static bool parse_instruction_line(Parser* parser, FnState* fx, Scan* scan) {
  if (!fx->active || fx->block_count == 0) {
    parse_fail(parser, "instruction outside of a .block");
    return false;
  }
  size_t block = fx->block_count - 1;
  if (fx->terminators[block]) {
    parse_fail(parser, "instructions after the block terminator");
    return false;
  }

  IrInstruction inst = {0};
  inst.result = IR_INVALID_ID;
  bool is_definition = false;
  size_t def_token = 0;
  bool ok = false;

  scan_ws(scan);
  if (scan->p[0] == 'v' && isdigit((unsigned char)scan->p[1])) {
    // Definition form: v<id> : <type> = <opcode> ...
    is_definition = true;
    scan->p++;
    if (!scan_id(scan, &def_token)) {
      parse_fail(parser, "invalid value id in definition");
      return false;
    }
    if (!scan_char(scan, ':')) {
      parse_fail(parser, "expected ':' after the value id");
      return false;
    }
    if (!parse_type(parser, scan, &inst.result_type))
      return false;
    if (!scan_char(scan, '=')) {
      parse_fail(parser, "expected '=' after the result type");
      return false;
    }
  }

  const char* word;
  size_t len;
  if (!scan_word(scan, &word, &len)) {
    parse_fail(parser, "expected an instruction");
    return false;
  }

  if (is_definition && parse_binary_opcode(word, len, &inst.type)) {
    if (!parse_operand(parser, scan, &inst.operands.binary.left))
      goto cleanup;
    if (!scan_char(scan, ',')) {
      parse_fail(parser, "expected ',' between binary operands");
      goto cleanup;
    }
    if (!parse_operand(parser, scan, &inst.operands.binary.right))
      goto cleanup;

    IrValueId result = ir_add_instruction(parser->ctx, fx->fid, block, inst);
    if (result == IR_INVALID_ID) {
      parse_fail(parser, "binary instructions always produce a value");
      goto cleanup;
    }
    ok = vmap_assign(parser, fx, def_token, result);
    goto cleanup;
  }

  if (is_definition && parse_unary_opcode(word, len, &inst.type)) {
    if (!parse_operand(parser, scan, &inst.operands.unary.operand))
      goto cleanup;

    IrValueId result = ir_add_instruction(parser->ctx, fx->fid, block, inst);
    if (result == IR_INVALID_ID) {
      parse_fail(parser, "unary instructions always produce a value");
      goto cleanup;
    }
    ok = vmap_assign(parser, fx, def_token, result);
    goto cleanup;
  }

  if (word_is(word, len, "load")) {
    if (!is_definition) {
      parse_fail(parser, "load produces a value: use 'v<id> : <type> = load @slot'");
      goto cleanup;
    }
    inst.type = IR_INST_LOAD;
    scan_ws(scan);
    if (*scan->p != '@') {
      parse_fail(parser, "expected '@slot' after 'load'");
      goto cleanup;
    }
    scan->p++;
    const char* name;
    size_t name_len;
    if (!parse_name(parser, scan, &name, &name_len))
      goto cleanup;
    if (!slot_lookup(parser, fx, name, name_len, &inst.operands.load.source))
      goto cleanup;

    IrValueId result = ir_add_instruction(parser->ctx, fx->fid, block, inst);
    if (result == IR_INVALID_ID) {
      parse_fail(parser, "load always produces a value");
      goto cleanup;
    }
    ok = vmap_assign(parser, fx, def_token, result);
    goto cleanup;
  }

  if (word_is(word, len, "store")) {
    if (is_definition) {
      parse_fail(parser,
                 "store never produces a value: use 'store @slot <operand>'");
      goto cleanup;
    }
    inst.type = IR_INST_STORE;
    inst.result_type = IR_UNIT;
    scan_ws(scan);
    if (*scan->p != '@') {
      parse_fail(parser, "expected '@slot' after 'store'");
      goto cleanup;
    }
    scan->p++;
    const char* name;
    size_t name_len;
    if (!parse_name(parser, scan, &name, &name_len))
      goto cleanup;
    if (!slot_lookup(parser, fx, name, name_len, &inst.operands.store.target))
      goto cleanup;
    if (!parse_operand(parser, scan, &inst.operands.store.source))
      goto cleanup;

    ir_add_instruction(parser->ctx, fx->fid, block, inst);
    ok = true;
    goto cleanup;
  }

  if (word_is(word, len, "call")) {
    const char* name;
    size_t name_len;
    if (!parse_name(parser, scan, &name, &name_len))
      goto cleanup;
    IrFunctionType call_type = IR_FX_USER;
    IrFunctionId call_id = IR_INVALID_ID;
    if (!resolve_function(parser, name, name_len, &call_type, &call_id))
      goto cleanup;

    // Collect arguments
    IrOperand* args = NULL;
    size_t arg_count = 0;
    size_t arg_capacity = 0;
    while (scan_char(scan, ',')) {
      if (arg_count + 1 > arg_capacity) {
        size_t capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
        IrOperand* grown = realloc(args, capacity * sizeof(IrOperand));
        if (grown == NULL) {
          parse_fail(parser, "out of memory");
          free(args);
          goto cleanup;
        }
        args = grown;
        arg_capacity = capacity;
      }
      if (!parse_operand(parser, scan, &args[arg_count])) {
        free(args);
        goto cleanup;
      }
      arg_count++;
    }

    inst.type = IR_INST_CALL;
    inst.result_type = is_definition ? inst.result_type : IR_UNIT;
    inst.operands.call.type = call_type;
    inst.operands.call.id = call_id;
    inst.operands.call.args = args;
    inst.operands.call.arg_count = arg_count;

    IrValueId result = ir_add_instruction(parser->ctx, fx->fid, block, inst);
    free(args);

    if (is_definition) {
      if (result == IR_INVALID_ID) {
        parse_fail(parser,
                   "this call returns unit and produces no value: "
                   "drop the 'v<id> : unit =' prefix");
        goto cleanup;
      }
      ok = vmap_assign(parser, fx, def_token, result);
    } else {
      ok = true;
    }
    goto cleanup;
  }

  // Terminators
  if (word_is(word, len, "jump") || word_is(word, len, "branch") ||
      word_is(word, len, "ret") || word_is(word, len, "exit")) {
    if (is_definition) {
      parse_fail(parser, "terminators never produce a value");
      goto cleanup;
    }

    IrBlockTerminator term = {0};
    if (word_is(word, len, "jump")) {
      term.type = IR_TERMINATOR_JUMP;
      if (!scan_id(scan, &term.value.jump_target)) {
        parse_fail(parser, "expected a block id after 'jump'");
        goto cleanup;
      }
    } else if (word_is(word, len, "branch")) {
      term.type = IR_TERMINATOR_CONDITIONAL;
      if (!parse_operand(parser, scan, &term.value.conditional_target.condition))
        goto cleanup;
      if (!scan_char(scan, '?')) {
        parse_fail(parser, "expected '?' in branch");
        free_operand(&term.value.conditional_target.condition);
        goto cleanup;
      }
      if (!scan_id(scan, &term.value.conditional_target.true_target)) {
        parse_fail(parser, "expected a block id after '?'");
        free_operand(&term.value.conditional_target.condition);
        goto cleanup;
      }
      if (!scan_char(scan, ':')) {
        parse_fail(parser, "expected ':' in branch");
        free_operand(&term.value.conditional_target.condition);
        goto cleanup;
      }
      if (!scan_id(scan, &term.value.conditional_target.false_target)) {
        parse_fail(parser, "expected a block id after ':'");
        free_operand(&term.value.conditional_target.condition);
        goto cleanup;
      }
    } else if (word_is(word, len, "ret")) {
      term.type = IR_TERMINATOR_RETURN;
      if (!parse_operand(parser, scan, &term.value.return_value))
        goto cleanup;
    } else {
      term.type = IR_TERMINATOR_EXIT;
      if (!parse_operand(parser, scan, &term.value.exit_code))
        goto cleanup;
    }

    ir_set_terminator(parser->ctx, fx->fid, block, term);
    fx->terminators[block] = true;
    ok = true;

    // The IR copied string operands; release the originals
    switch (term.type) {
      case IR_TERMINATOR_CONDITIONAL:
        free_operand(&term.value.conditional_target.condition);
        break;
      case IR_TERMINATOR_RETURN:
        free_operand(&term.value.return_value);
        break;
      case IR_TERMINATOR_EXIT:
        free_operand(&term.value.exit_code);
        break;
      default:
        break;
    }
    goto cleanup;
  }

  parse_fail(parser, "unknown instruction '%.*s'", (int)(len > 31 ? 31 : len), word);

cleanup:
  // Free temporary string operands (the IR made its own copies). For call
  // instructions the argument array was already released where it was owned.
  switch (inst.type) {
    case IR_INST_STORE:
      free_operand(&inst.operands.store.source);
      break;
    case IR_INST_ADD:
    case IR_INST_SUB:
    case IR_INST_MUL:
    case IR_INST_IDIV:
    case IR_INST_DIV:
    case IR_INST_EQ:
    case IR_INST_NE:
    case IR_INST_LT:
    case IR_INST_LE:
    case IR_INST_GT:
    case IR_INST_GE:
    case IR_INST_CONCAT:
      free_operand(&inst.operands.binary.left);
      free_operand(&inst.operands.binary.right);
      break;
    case IR_INST_NOT:
    case IR_INST_INT2DOUBLE:
    case IR_INST_FLOOR:
      free_operand(&inst.operands.unary.operand);
      break;
    default:
      break;
  }
  return ok;
}

/**
 * @brief Strips comments (string-aware) and trailing whitespace from a line
 */
static void strip_line(char* line) {
  bool in_string = false;
  for (char* c = line; *c != '\0'; c++) {
    if (in_string) {
      if (*c == '\\' && c[1] != '\0')
        c++;  // Skip the escaped character
      else if (*c == '"')
        in_string = false;
    } else if (*c == '"') {
      in_string = true;
    } else if (*c == '#') {
      *c = '\0';
      break;
    }
  }

  // Right-trim whitespace and carriage returns
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                     line[len - 1] == '\r'))
    line[--len] = '\0';
}

/**
 * @brief Splits the text into meaningful lines (comments/blanks removed)
 */
static bool prepare_lines(Parser* parser, const char* text, char** buffer_out,
                          Line** lines_out, size_t* count_out) {
  size_t length = strlen(text);
  char* buffer = malloc(length + 2);
  if (buffer == NULL) {
    parse_fail(parser, "out of memory");
    return false;
  }
  memcpy(buffer, text, length + 1);

  Line* lines = NULL;
  size_t count = 0;
  size_t capacity = 0;
  int line_no = 0;
  bool ok = true;

  char* cursor = buffer;
  while (ok) {
    line_no++;

    char* newline = strchr(cursor, '\n');
    if (newline != NULL)
      *newline = '\0';

    strip_line(cursor);
    if (*cursor != '\0') {
      if (count + 1 > capacity) {
        size_t new_capacity = capacity == 0 ? 64 : capacity * 2;
        Line* grown = realloc(lines, new_capacity * sizeof(Line));
        if (grown == NULL) {
          parse_fail(parser, "out of memory");
          ok = false;
          break;
        }
        lines = grown;
        capacity = new_capacity;
      }
      lines[count].text = cursor;
      lines[count].no = line_no;
      count++;
    }

    if (newline == NULL)
      break;
    cursor = newline + 1;
  }

  if (!ok) {
    free(lines);
    free(buffer);
    return false;
  }

  *buffer_out = buffer;
  *lines_out = lines;
  *count_out = count;
  return true;
}

bool ir_text_parse(Ctx* ctx, const char* text, char* error_buf, size_t error_size) {
  Parser parser = {
      .ctx = ctx,
      .error = error_buf,
      .error_size = error_size,
      .failed = false,
      .current_line = 0,
      .fnames_names = NULL,
      .fnames_ids = NULL,
      .function_count = 0,
      .fnames_capacity = 0,
  };
  if (error_size > 0)
    error_buf[0] = '\0';

  FnState fx = {0};
  size_t func_index = 0;

  char* buffer = NULL;
  Line* lines = NULL;
  size_t line_count = 0;

  const char* entrypoint_name = NULL;
  bool has_entrypoint = false;

  // Replace any IR currently held by the context
  ir_free(&ctx->ir);
  ir_init(&ctx->ir);

  if (!prepare_lines(&parser, text, &buffer, &lines, &line_count))
    goto done;

  ////////////////////////////////////////////////////////////////////////////
  // Pass 1: create function shells and record the program entrypoint
  ////////////////////////////////////////////////////////////////////////////
  for (size_t i = 0; i < line_count && !parser.failed; i++) {
    parser.current_line = lines[i].no;
    char* line = lines[i].text;
    if (line[0] != '.')
      continue;

    Scan scan = {line + 1};  // Skip the leading '.'
    const char* directive;
    size_t directive_len;
    if (!scan_word(&scan, &directive, &directive_len)) {
      parse_fail(&parser, "expected a directive after '.'");
      break;
    }

    if (word_is(directive, directive_len, "func")) {
      const char* name;
      size_t name_len;
      IrType return_type;
      if (!parse_func_header(&parser, &scan, &name, &name_len, &return_type))
        break;

      // NUL-terminate the name in the line buffer (the header is fully
      // parsed, so nothing else reads past the name)
      char* mutable_name = (char*)name;
      mutable_name[name_len] = '\0';

      bool duplicate = false;
      for (size_t f = 0; f < parser.function_count; f++) {
        if (strcmp(parser.fnames_names[f], mutable_name) == 0) {
          parse_fail(&parser, "duplicate function '%s'", mutable_name);
          duplicate = true;
          break;
        }
      }
      if (duplicate)
        break;

      if (parser.function_count + 1 > parser.fnames_capacity) {
        size_t capacity = parser.fnames_capacity == 0 ? 8 : parser.fnames_capacity * 2;
        const char** grown_names = realloc(parser.fnames_names, capacity * sizeof(char*));
        IrFunctionId* grown_ids = realloc(parser.fnames_ids, capacity * sizeof(IrFunctionId));
        if (grown_names == NULL || grown_ids == NULL) {
          free(grown_names);
          parse_fail(&parser, "out of memory");
          break;
        }
        parser.fnames_names = grown_names;
        parser.fnames_ids = grown_ids;
        parser.fnames_capacity = capacity;
      }

      parser.fnames_names[parser.function_count] = mutable_name;
      parser.fnames_ids[parser.function_count] =
          ir_add_function(ctx, mutable_name, return_type);
      parser.function_count++;
      continue;
    }

    if (word_is(directive, directive_len, "entrypoint")) {
      const char* name;
      size_t name_len;
      if (!parse_name(&parser, &scan, &name, &name_len)) {
        parse_fail(&parser, "expected a function name after '.entrypoint'");
        break;
      }
      if (!scan_end(&parser, &scan))
        break;
      if (has_entrypoint) {
        parse_fail(&parser, "duplicate '.entrypoint' directive");
        break;
      }
      char* mutable_name = (char*)name;
      mutable_name[name_len] = '\0';
      entrypoint_name = mutable_name;
      has_entrypoint = true;
      continue;
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // Pass 2: parse function bodies
  ////////////////////////////////////////////////////////////////////////////
  for (size_t i = 0; i < line_count && !parser.failed; i++) {
    parser.current_line = lines[i].no;
    char* line = lines[i].text;
    Scan scan = {line};

    if (line[0] == '.') {
      Scan dscan = {line + 1};  // Skip the leading '.'
      const char* directive;
      size_t directive_len;
      if (!scan_word(&dscan, &directive, &directive_len)) {
        parse_fail(&parser, "expected a directive after '.'");
        break;
      }
      scan = dscan;

      if (word_is(directive, directive_len, "func")) {
        // Close the previous function and open the next one; functions were
        // created in pass 1 in this exact order
        parser.current_line = 0;
        if (!finish_function(&parser, &fx))
          break;
        parser.current_line = lines[i].no;

        if (func_index >= parser.function_count)
          break;  // unreachable: pass 1 saw the same lines
        fx.fid = parser.fnames_ids[func_index++];
        fx.active = true;
        fx.block_count = 0;
        fx.saw_local = false;
        fx.has_entry = false;
        fx.entry = 0;
        continue;
      }

      if (word_is(directive, directive_len, "entrypoint"))
        continue;  // handled in pass 1

      if (word_is(directive, directive_len, "param")) {
        if (!parse_slot_directive(&parser, &fx, &scan, true))
          break;
        continue;
      }

      if (word_is(directive, directive_len, "local")) {
        if (!parse_slot_directive(&parser, &fx, &scan, false))
          break;
        continue;
      }

      if (word_is(directive, directive_len, "entry")) {
        if (!fx.active) {
          parse_fail(&parser, "'.entry' outside of a function");
          break;
        }
        if (fx.has_entry) {
          parse_fail(&parser, "duplicate '.entry' directive");
          break;
        }
        if (!scan_id(&scan, &fx.entry)) {
          parse_fail(&parser, "expected a block id after '.entry'");
          break;
        }
        if (!scan_end(&parser, &scan))
          break;
        fx.has_entry = true;
        continue;
      }

      if (word_is(directive, directive_len, "block")) {
        if (!fx.active) {
          parse_fail(&parser, "'.block' outside of a function");
          break;
        }
        if (!parse_block_directive(&parser, &fx, &scan))
          break;
        continue;
      }

      parse_fail(&parser, "unknown directive '%.*s'",
                 (int)(directive_len > 31 ? 31 : directive_len), directive);
      break;
    }

    // Instruction or terminator line
    Scan inst_scan = {line};
    if (!parse_instruction_line(&parser, &fx, &inst_scan))
      break;
  }

  if (!parser.failed) {
    parser.current_line = 0;
    if (!finish_function(&parser, &fx))
      goto done;

    if (parser.function_count == 0) {
      parse_fail(&parser, "no functions defined");
      goto done;
    }

    // Resolve the program entrypoint
    if (has_entrypoint) {
      bool found = false;
      for (size_t f = 0; f < parser.function_count; f++) {
        if (strcmp(parser.fnames_names[f], entrypoint_name) == 0) {
          ctx->ir.entrypoint = parser.fnames_ids[f];
          found = true;
          break;
        }
      }
      if (!found) {
        parse_fail(&parser, ".entrypoint references undefined function '%s'",
                   entrypoint_name);
        goto done;
      }
    } else {
      ctx->ir.entrypoint = 0;
    }
  }

done:
  free(fx.vmap);
  free(fx.terminators);
  free(fx.slot_names);
  free(parser.fnames_names);
  free(parser.fnames_ids);
  free(lines);
  free(buffer);

  return !parser.failed;
}
