/**
 * @file ifj26_opt.c
 * @brief Standalone optimizer driver for the IFJ26 playground
 *
 * Usage:
 *   ifj26-opt [--plain] [--no-opt] [--verbose] < input.ir
 *
 * Reads textual IR (see ir_text.h) from stdin, parses it into the shared
 * context, runs the repository's optimize_ir() and prints the optimized IR.
 * By default the result is wrapped in a JSON envelope consumed by the web
 * server; --plain prints the raw IR text instead (handy for CLI use).
 *
 * Exit codes: 0 success, 1 parse error, 2 CLI usage error, 99 internal error
 * (mirrors the compiler's E_INTERNAL_ERROR).
 */

#include "ctx.h"        // Ctx, Flags, THROW machinery
#include "ir_text.h"    // ir_text_parse, ir_text_print
#include "optimizer.h"  // optimize_ir

#include <setjmp.h>   // setjmp, longjmp (through ctx.h)
#include <stdbool.h>  // bool
#include <stddef.h>   // size_t
#include <stdio.h>    // printf, fprintf, fread, stdin, stdout, stderr
#include <stdlib.h>   // malloc, realloc, free, strcmp
#include <string.h>   // strcmp

#ifdef _WIN32
#include <fcntl.h>  // _O_BINARY
#include <io.h>     // _setmode, _fileno
#endif

// Static to survive long jumps; referenced from the error path
static bool g_plain = false;    // --plain: raw IR instead of a JSON envelope
static bool g_no_opt = false;   // --no-opt: parse + print without optimizing
static bool g_verbose = false;  // --verbose: enable compiler debug output
static char* g_input = NULL;    // Whole IR text read from stdin

/**
 * @brief Prints the CLI usage information to stderr
 */
static void print_usage(void) {
  fprintf(stderr,
          "Usage: ifj26-opt [--plain] [--no-opt] [--verbose] < input.ir\n"
          "\n"
          "Reads textual IR from stdin, runs the optimizer and writes the\n"
          "result to stdout.\n"
          "  --plain    write plain IR text instead of a JSON envelope\n"
          "  --no-opt   parse and print without optimizing (round-trip check)\n"
          "  --verbose  forward the compiler's debug output to stderr\n");
}

/**
 * @brief Reads the whole of stdin into a freshly allocated buffer
 *
 * @param[out] out_len Length of the read data
 * @return The NUL-terminated data, or NULL on allocation failure
 */
static char* read_all_stdin(size_t* out_len) {
  size_t capacity = 65536;
  size_t length = 0;
  char* data = malloc(capacity);
  if (data == NULL)
    return NULL;

  for (;;) {
    if (length + 1 >= capacity) {
      capacity *= 2;
      char* grown = realloc(data, capacity);
      if (grown == NULL) {
        free(data);
        return NULL;
      }
      data = grown;
    }

    size_t read = fread(data + length, 1, capacity - 1 - length, stdin);
    length += read;
    if (read == 0)
      break;  // EOF or read error
  }

  data[length] = '\0';
  *out_len = length;
  return data;
}

/**
 * @brief Prints a string as a JSON string literal (including the quotes)
 */
static void json_print_string(const char* str) {
  putchar('"');
  for (const unsigned char* c = (const unsigned char*)str; *c != '\0'; c++) {
    switch (*c) {
      case '"':
        printf("\\\"");
        break;
      case '\\':
        printf("\\\\");
        break;
      case '\b':
        printf("\\b");
        break;
      case '\f':
        printf("\\f");
        break;
      case '\n':
        printf("\\n");
        break;
      case '\r':
        printf("\\r");
        break;
      case '\t':
        printf("\\t");
        break;
      default:
        if (*c < 0x20)
          printf("\\u%04x", *c);
        else
          putchar(*c);
        break;
    }
  }
  putchar('"');
}

/**
 * @brief Counts all instructions in the IR (for the before/after stats)
 */
static size_t count_instructions(Ctx* ctx) {
  size_t count = 0;
  for (size_t f = 0; f < ctx->ir.functions.count; f++) {
    IrFunction* fx = &ctx->ir.functions.data[f];
    for (size_t b = 0; b < fx->blocks.count; b++)
      count += fx->blocks.data[b].instructions.count;
  }
  return count;
}

/**
 * @brief Entrypoint of the ifj26-opt tool
 */
int main(int argc, char** argv) {
#ifdef _WIN32
  // Binary mode so piped IR and JSON keep predictable byte endings
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  // Parse the tool's own CLI flags
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--plain") == 0)
      g_plain = true;
    else if (strcmp(argv[i], "--no-opt") == 0)
      g_no_opt = true;
    else if (strcmp(argv[i], "--verbose") == 0)
      g_verbose = true;
    else {
      fprintf(stderr, "Unknown argument: %s\n\n", argv[i]);
      print_usage();
      return 2;
    }
  }

  // Read the IR text from stdin before touching the compiler context
  size_t input_len = 0;
  g_input = read_all_stdin(&input_len);
  if (g_input == NULL) {
    fprintf(stderr, "error: out of memory while reading stdin\n");
    return 99;
  }

  // Initialize shared memory and error context (same pattern as ifj26.c)
  static Ctx context;  // Static to survive long jump
  Flags flags = DEFAULT_FLAGS;
  flags.verbose = g_verbose;
  ctx_init(&context, flags);

  // If an error is thrown at any point (allocation failure inside the parser,
  // optimizer or printer), print the envelope and exit with the thrown code
  int jump_result = setjmp(context.error_target);
  if (jump_result != 0) {
    int code = (int)context.exit_code;
    if (!g_plain)
      printf("{\"ok\":false,\"error\":\"internal error (exit code %d)\"}\n", code);
    free(g_input);
    ctx_free(&context);
    return code != 0 ? code : 99;
  }

  ctx_allocate(&context);

  // Parse the textual IR
  char error[256];
  if (!ir_text_parse(&context, g_input, error, sizeof(error))) {
    if (g_plain)
      fprintf(stderr, "error: %s\n", error);
    else {
      printf("{\"ok\":false,\"error\":");
      json_print_string(error);
      printf("}\n");
    }
    free(g_input);
    ctx_free(&context);
    return 1;
  }

  // Run the optimizer (unless disabled for round-trip checking)
  size_t instructions_before = count_instructions(&context);
  if (!g_no_opt)
    optimize_ir(&context);
  size_t instructions_after = count_instructions(&context);

  // Print the optimized IR
  char* text = ir_text_print(&context);
  if (text == NULL) {
    if (g_plain)
      fprintf(stderr, "error: out of memory while printing\n");
    else
      printf("{\"ok\":false,\"error\":\"out of memory while printing\"}\n");
    free(g_input);
    ctx_free(&context);
    return 99;
  }

  if (g_plain) {
    fputs(text, stdout);
  } else {
    printf("{\"ok\":true,\"ir\":");
    json_print_string(text);
    printf(",\"stats\":{\"instructions_before\":%zu,\"instructions_after\":%zu}}\n",
           instructions_before, instructions_after);
  }

  free(text);
  free(g_input);
  return (int)ctx_free(&context);
}
