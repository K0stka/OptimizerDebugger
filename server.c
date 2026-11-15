/**
 * @file server.c
 * @brief Local dev server for the IFJ26 optimizer playground
 *
 * Responsibilities:
 *  - serve the static page from static/ and presets from presets/
 *  - build the ifj26-opt tool from the repository's src C files (minus
 *    ifj26.c, which owns the compiler's main) plus opt_src C files into
 *    build/<flavor>/ifj26-opt
 *  - rebuild automatically whenever any input source is newer than the binary
 *    (hot reload: save a file, press Optimize, get the fresh optimizer)
 *  - spawn ifj26-opt as a fresh process per optimize request, piping the IR
 *    text to its stdin and relaying its JSON envelope to the browser
 *
 * Endpoints:
 *  GET  /                static/index.html
 *  GET  /app.js          static/app.js
 *  GET  /style.css       static/style.css
 *  GET  /api/presets     JSON array of preset names
 *  GET  /presets/<name>  preset IR text
 *  GET  /api/status      repository + build state JSON
 *  POST /api/optimize?flavor=release|debug   body: IR text
 *  POST /api/rebuild?flavor=release|debug    force a rebuild
 *
 * Run from the project root (make run). Configuration comes from config.txt,
 * overridable with --repo / --port. Use --build-only to just build both
 * flavors and exit (make opt).
 */

#include <stdarg.h>   // va_list, va_start, va_end
#include <stdbool.h>  // bool
#include <stdint.h>   // int64_t, UINT64_MAX
#include <stdio.h>    // printf, fopen, snprintf
#include <stdlib.h>   // malloc, free, atoi, qsort
#include <string.h>   // strcmp, strncmp, strstr, strlen
#include <time.h>     // time, strftime (request log)

#ifdef _WIN32
#include <winsock2.h>  // sockets (must precede windows.h)
#include <ws2tcpip.h>  // sockaddr
#include <windows.h>   // CreateProcess, FindFirstFile, threads
#include <process.h>   // _beginthreadex

typedef SOCKET socket_t;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#define EXE_SUFFIX ".exe"
#define strncasecmp _strnicmp
#else
#include <dirent.h>     // opendir, readdir
#include <fcntl.h>      // fcntl
#include <pthread.h>    // reader thread
#include <sys/socket.h> // socket, bind, listen, accept, recv, send
#include <sys/stat.h>   // stat, mkdir
#include <sys/types.h>  // pid_t
#include <sys/wait.h>   // waitpid
#include <unistd.h>     // close, read, write, fork, execl, getcwd

typedef int socket_t;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#define EXE_SUFFIX ""
#endif

/// Maximum accepted request header size
#define MAX_HEADER_SIZE (64 * 1024)
/// Maximum accepted request body size (IR text)
#define MAX_BODY_SIZE (8 * 1024 * 1024)
/// Hard cap on the build log kept for the UI
#define MAX_LOG_SIZE (48 * 1024)

////////////////////////////////////////////////////////////////////////////////
// Dynamic buffer
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Growable byte buffer
 */
typedef struct {
  char* data;      // NUL-terminated contents (NULL while empty)
  size_t len;      // Current length
  size_t capacity; // Allocated bytes
} Buf;

/**
 * @brief Appends count bytes to the buffer; returns false on allocation failure
 */
static bool buf_append(Buf* buf, const char* data, size_t count) {
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
static bool buf_puts(Buf* buf, const char* str) {
  return buf_append(buf, str, strlen(str));
}

/**
 * @brief Appends formatted data to the buffer
 */
static bool buf_printf(Buf* buf, const char* format, ...) {
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

  char stack_buffer[256];
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

/**
 * @brief Releases the buffer's storage
 */
static void buf_free(Buf* buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->capacity = 0;
}

/**
 * @brief Appends a string as a JSON string literal (with quotes), escaping
 *        quotes, backslashes and control characters
 */
static bool buf_put_json_string(Buf* buf, const char* str) {
  if (!buf_puts(buf, "\""))
    return false;

  for (const unsigned char* c = (const unsigned char*)(str == NULL ? "" : str);
       *c != '\0'; c++) {
    bool ok = true;
    switch (*c) {
      case '"':
        ok = buf_puts(buf, "\\\"");
        break;
      case '\\':
        ok = buf_puts(buf, "\\\\");
        break;
      case '\b':
        ok = buf_puts(buf, "\\b");
        break;
      case '\f':
        ok = buf_puts(buf, "\\f");
        break;
      case '\n':
        ok = buf_puts(buf, "\\n");
        break;
      case '\r':
        ok = buf_puts(buf, "\\r");
        break;
      case '\t':
        ok = buf_puts(buf, "\\t");
        break;
      default:
        if (*c < 0x20)
          ok = buf_printf(buf, "\\u%04x", *c);
        else
          ok = buf_append(buf, (const char*)c, 1);
        break;
    }
    if (!ok)
      return false;
  }

  return buf_puts(buf, "\"");
}

////////////////////////////////////////////////////////////////////////////////
// String list
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Simple list of heap-owned strings
 */
typedef struct {
  char** items;
  size_t count;
  size_t capacity;
} StrList;

/**
 * @brief Appends a copy of str to the list
 */
static bool strlist_add(StrList* list, const char* str) {
  if (list->count + 1 > list->capacity) {
    size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    char** grown = realloc(list->items, capacity * sizeof(char*));
    if (grown == NULL)
      return false;
    list->items = grown;
    list->capacity = capacity;
  }

  size_t len = strlen(str);
  char* copy = malloc(len + 1);
  if (copy == NULL)
    return false;
  memcpy(copy, str, len + 1);
  list->items[list->count++] = copy;
  return true;
}

/**
 * @brief Releases the list and its strings
 */
static void strlist_free(StrList* list) {
  for (size_t i = 0; i < list->count; i++)
    free(list->items[i]);
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

/**
 * @brief qsort comparator for string lists
 */
static int strlist_cmp(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

////////////////////////////////////////////////////////////////////////////////
// Filesystem helpers
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Returns whether a directory exists
 */
static bool dir_exists(const char* path) {
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/**
 * @brief Returns the file's last-write time in seconds, or -1 if it is missing
 */
static int64_t file_mtime(const char* path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
    return -1;
  ULARGE_INTEGER combined = {.HighPart = data.ftLastWriteTime.dwHighDateTime,
                             .LowPart = data.ftLastWriteTime.dwLowDateTime};
  return (int64_t)(combined.QuadPart / 10000000ull);  // 100ns -> seconds
#else
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (int64_t)st.st_mtime;
#endif
}

/**
 * @brief Creates a directory and all missing parents; false on failure
 */
static bool mkdir_p(const char* path) {
  char tmp[1024];
  size_t len = strlen(path);
  if (len >= sizeof(tmp))
    return false;
  memcpy(tmp, path, len + 1);

  for (char* p = tmp + 1; *p != '\0'; p++) {
    if (*p != '/' && *p != '\\')
      continue;
    char saved = *p;
    *p = '\0';
#ifdef _WIN32
    CreateDirectoryA(tmp, NULL);  // Already-existing dirs are fine
#else
    mkdir(tmp, 0755);
#endif
    *p = saved;
  }

#ifdef _WIN32
  CreateDirectoryA(tmp, NULL);
#else
  mkdir(tmp, 0755);
#endif
  return dir_exists(tmp);
}

/**
 * @brief Reads a whole file into the buffer; false if it cannot be read
 */
static bool read_whole_file(const char* path, Buf* out) {
  FILE* file = fopen(path, "rb");
  if (file == NULL)
    return false;

  bool ok = true;
  char chunk[8192];
  size_t read;
  while ((read = fread(chunk, 1, sizeof(chunk), file)) > 0) {
    if (!buf_append(out, chunk, read)) {
      ok = false;
      break;
    }
  }

  fclose(file);
  return ok;
}

/**
 * @brief Lists files in dir matching the (optional) suffix, skipping dotfiles
 */
static bool list_dir_files(const char* dir, const char* suffix, StrList* out) {
#ifdef _WIN32
  char pattern[1200];
  snprintf(pattern, sizeof(pattern), "%s\\*", dir);

  WIN32_FIND_DATAA found;
  HANDLE handle = FindFirstFileA(pattern, &found);
  if (handle == INVALID_HANDLE_VALUE)
    return false;

  do {
    if (found.cFileName[0] == '.')
      continue;
    if (suffix != NULL) {
      size_t name_len = strlen(found.cFileName);
      size_t suffix_len = strlen(suffix);
      if (name_len < suffix_len ||
          strcmp(found.cFileName + name_len - suffix_len, suffix) != 0)
        continue;
    }
    if (!strlist_add(out, found.cFileName)) {
      FindClose(handle);
      return false;
    }
  } while (FindNextFileA(handle, &found));

  FindClose(handle);
  return true;
#else
  DIR* dirp = opendir(dir);
  if (dirp == NULL)
    return false;

  struct dirent* entry;
  while ((entry = readdir(dirp)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    if (suffix != NULL) {
      size_t name_len = strlen(entry->d_name);
      size_t suffix_len = strlen(suffix);
      if (name_len < suffix_len ||
          strcmp(entry->d_name + name_len - suffix_len, suffix) != 0)
        continue;
    }
    if (!strlist_add(out, entry->d_name)) {
      closedir(dirp);
      return false;
    }
  }

  closedir(dirp);
  return true;
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Monotonic clock + logging
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Milliseconds on a monotonic clock (for build timing)
 */
static double now_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart * 1000.0;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

/**
 * @brief Prints a timestamped line to stdout
 */
static void log_line(const char* format, ...) {
  char timestamp[16];
  time_t now = time(NULL);
  struct tm* local = localtime(&now);
  strftime(timestamp, sizeof(timestamp), "%H:%M:%S", local);

  printf("[%s] ", timestamp);
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  printf("\n");
  fflush(stdout);
}

////////////////////////////////////////////////////////////////////////////////
// Configuration + global state
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Server configuration (config.txt + CLI overrides)
 */
typedef struct {
  char repo[1024];  // IFJ26 repository root
  int port;         // Listen port
} Config;

static Config g_config = {.repo = "../IFJ26", .port = 8080};
static char g_root[1024];  // Playground root directory (current directory)

/// Build flavors: 0 = release (-O3), 1 = debug (-g -O0)
static const char* const FLAVOR_NAMES[] = {"release", "debug"};
#define FLAVOR_COUNT 2

/**
 * @brief Result of the last build attempt per flavor (for /api/status)
 */
typedef struct {
  bool valid;  // A build was attempted in this server's lifetime
  bool ok;
  double ms;
} LastBuild;

static LastBuild g_last_build[FLAVOR_COUNT];

/**
 * @brief Loads key = value pairs from config.txt (CLI overrides applied later)
 */
static void load_config(const char* path) {
  FILE* file = fopen(path, "r");
  if (file == NULL)
    return;

  char line[2048];
  while (fgets(line, sizeof(line), file) != NULL) {
    // Strip comments and whitespace
    char* hash = strchr(line, '#');
    if (hash != NULL)
      *hash = '\0';

    char* key = line;
    while (*key == ' ' || *key == '\t' || *key == '\r' || *key == '\n')
      key++;
    char* equals = strchr(key, '=');
    if (equals == NULL)
      continue;
    *equals = '\0';

    char* value = equals + 1;
    char* end = key + strlen(key);
    while (end > key && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                         end[-1] == '\n'))
      *--end = '\0';

    while (*value == ' ' || *value == '\t')
      value++;
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                           end[-1] == '\n'))
      *--end = '\0';

    if (strcmp(key, "repo") == 0 && *value != '\0') {
      size_t len = strlen(value);
      if (len < sizeof(g_config.repo))
        memcpy(g_config.repo, value, len + 1);
    } else if (strcmp(key, "port") == 0 && *value != '\0') {
      g_config.port = atoi(value);
    }
  }

  fclose(file);
}

/**
 * @brief Returns whether the configured repository looks valid
 */
static bool repo_ok(void) {
  char src[1200];
  snprintf(src, sizeof(src), "%s/src", g_config.repo);
  return dir_exists(src);
}

////////////////////////////////////////////////////////////////////////////////
// Process runner
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Result of a spawned subprocess
 */
typedef struct {
  bool spawned;   // Whether the process was created at all
  int exit_code;
  Buf out;  // stdout
  Buf err;  // stderr
} ProcResult;

/**
 * @brief Releases a process result's buffers
 */
static void proc_result_free(ProcResult* result) {
  buf_free(&result->out);
  buf_free(&result->err);
}

#ifdef _WIN32

/**
 * @brief Thread argument for the stdout reader
 */
typedef struct {
  HANDLE handle;
  Buf* target;
} ReaderArg;

/**
 * @brief Reads a pipe into a buffer until EOF (runs on a helper thread)
 */
static unsigned __stdcall reader_thread(void* argument) {
  ReaderArg* arg = (ReaderArg*)argument;
  char chunk[4096];
  DWORD read = 0;
  while (ReadFile(arg->handle, chunk, sizeof(chunk), &read, NULL) && read > 0) {
    if (!buf_append(arg->target, chunk, read))
      break;
  }
  return 0;
}

/**
 * @brief Spawns cmdline (CreateProcess syntax), pipes stdin, captures stdout/stderr
 */
static bool run_process(const char* cmdline, const char* stdin_data,
                        size_t stdin_len, ProcResult* result) {
  result->spawned = false;
  result->exit_code = -1;

  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
  HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
  HANDLE err_read = NULL, err_write = NULL;

  if (!CreatePipe(&in_read, &in_write, &inherit, 0) ||
      !CreatePipe(&out_read, &out_write, &inherit, 0) ||
      !CreatePipe(&err_read, &err_write, &inherit, 0)) {
    buf_puts(&result->err, "CreatePipe failed");
    return false;
  }

  // Our ends must not be inherited
  SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA startup = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = in_read;
  startup.hStdOutput = out_write;
  startup.hStdError = err_write;

  PROCESS_INFORMATION process = {0};
#ifndef CREATE_NO_WINDOW
#define CREATE_NO_WINDOW 0x08000000
#endif
  BOOL created = CreateProcessA(NULL, (LPSTR)cmdline, NULL, NULL, TRUE,
                                CREATE_NO_WINDOW, NULL, NULL, &startup, &process);

  // Close the child ends we do not use
  CloseHandle(in_read);
  CloseHandle(out_write);
  CloseHandle(err_write);

  if (!created) {
    buf_printf(&result->err, "CreateProcess failed (error %lu)", GetLastError());
    CloseHandle(in_write);
    CloseHandle(out_read);
    CloseHandle(err_read);
    return false;
  }
  result->spawned = true;

  // Drain stdout on a helper thread so a chatty child cannot block us
  ReaderArg reader_arg = {.handle = out_read, .target = &result->out};
  uintptr_t reader = _beginthreadex(NULL, 0, reader_thread, &reader_arg, 0, NULL);

  // Feed stdin (clang receives none; ifj26-opt always consumes it fully)
  if (stdin_data != NULL && stdin_len > 0) {
    size_t written = 0;
    while (written < stdin_len) {
      DWORD chunk = (DWORD)((stdin_len - written) > 0x40000000u
                                ? 0x40000000u
                                : (stdin_len - written));
      DWORD wrote = 0;
      if (!WriteFile(in_write, stdin_data + written, chunk, &wrote, NULL) ||
          wrote == 0)
        break;
      written += wrote;
    }
  }
  CloseHandle(in_write);

  // Drain stderr on this thread
  char chunk[4096];
  DWORD read = 0;
  while (ReadFile(err_read, chunk, sizeof(chunk), &read, NULL) && read > 0) {
    if (!buf_append(&result->err, chunk, read))
      break;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);

  if (reader != 0) {
    WaitForSingleObject((HANDLE)reader, INFINITE);
    CloseHandle((HANDLE)reader);
  }

  CloseHandle(out_read);
  CloseHandle(err_read);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);

  result->exit_code = (int)exit_code;
  return true;
}

#else  // POSIX

/**
 * @brief Thread argument for the stdout reader
 */
typedef struct {
  int fd;
  Buf* target;
} ReaderArg;

/**
 * @brief Reads a pipe into a buffer until EOF (runs on a helper thread)
 */
static void* reader_thread(void* argument) {
  ReaderArg* arg = (ReaderArg*)argument;
  char chunk[4096];
  ssize_t nread;
  while ((nread = read(arg->fd, chunk, sizeof(chunk))) > 0) {
    if (!buf_append(arg->target, chunk, (size_t)nread))
      break;
  }
  return NULL;
}

/**
 * @brief Spawns cmdline via /bin/sh, pipes stdin, captures stdout/stderr
 */
static bool run_process(const char* cmdline, const char* stdin_data,
                        size_t stdin_len, ProcResult* result) {
  result->spawned = false;
  result->exit_code = -1;

  int in_pipe[2], out_pipe[2], err_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    buf_puts(&result->err, "pipe() failed");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    buf_puts(&result->err, "fork() failed");
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return false;
  }

  if (pid == 0) {
    // Child: wire the pipes to std streams and exec
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    execl("/bin/sh", "sh", "-c", cmdline, (char*)NULL);
    _exit(127);
  }

  // Parent: close the child ends
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);
  result->spawned = true;

  // Drain stdout on a helper thread so a chatty child cannot block us
  ReaderArg reader_arg = {.fd = out_pipe[0], .target = &result->out};
  pthread_t reader;
  bool reader_started =
      pthread_create(&reader, NULL, reader_thread, &reader_arg) == 0;

  // Feed stdin
  if (stdin_data != NULL && stdin_len > 0) {
    size_t written = 0;
    while (written < stdin_len) {
      ssize_t wrote = write(in_pipe[1], stdin_data + written, stdin_len - written);
      if (wrote <= 0)
        break;
      written += (size_t)wrote;
    }
  }
  close(in_pipe[1]);

  // Drain stderr on this thread
  char chunk[4096];
  ssize_t nread;
  while ((nread = read(err_pipe[0], chunk, sizeof(chunk))) > 0) {
    if (!buf_append(&result->err, chunk, (size_t)nread))
      break;
  }

  int status = 0;
  waitpid(pid, &status, 0);
  result->exit_code =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

  if (reader_started)
    pthread_join(reader, NULL);
  else
    close(out_pipe[0]);

  close(err_pipe[0]);
  return true;
}

#endif

////////////////////////////////////////////////////////////////////////////////
// Build system (the single source of truth for how ifj26-opt is compiled)
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Returns the path of a built binary for the flavor
 */
static void binary_path(int flavor, char* out, size_t out_size) {
#ifdef _WIN32
  snprintf(out, out_size, "build\\%s\\ifj26-opt%s", FLAVOR_NAMES[flavor],
           EXE_SUFFIX);
#else
  snprintf(out, out_size, "build/%s/ifj26-opt%s", FLAVOR_NAMES[flavor],
           EXE_SUFFIX);
#endif
}

/**
 * @brief Collects the build inputs: .c sources and .h headers
 *
 * Sources cover the repository's src C files (excluding ifj26.c, which owns
 * the compiler's main) plus opt_src C files. Headers cover both directories'
 * .h files. Returns
 * false when the repository does not exist.
 */
static bool collect_build_inputs(StrList* sources, StrList* headers) {
  if (!repo_ok())
    return false;

  char dir[1200];

  // Repository sources (skip ifj26.c: it has the compiler's main())
  snprintf(dir, sizeof(dir), "%s/src", g_config.repo);
  StrList repo_sources = {0};
  if (!list_dir_files(dir, ".c", &repo_sources))
    return false;
  qsort(repo_sources.items, repo_sources.count, sizeof(char*), strlist_cmp);
  for (size_t i = 0; i < repo_sources.count; i++) {
    if (strcmp(repo_sources.items[i], "ifj26.c") == 0)
      continue;
    char path[1400];
    snprintf(path, sizeof(path), "%s/src/%s", g_config.repo, repo_sources.items[i]);
    if (!strlist_add(sources, path)) {
      strlist_free(&repo_sources);
      return false;
    }
  }
  strlist_free(&repo_sources);

  // Repository headers (staleness tracking)
  StrList repo_headers = {0};
  snprintf(dir, sizeof(dir), "%s/src", g_config.repo);
  if (list_dir_files(dir, ".h", &repo_headers)) {
    qsort(repo_headers.items, repo_headers.count, sizeof(char*), strlist_cmp);
    for (size_t i = 0; i < repo_headers.count; i++) {
      char path[1400];
      snprintf(path, sizeof(path), "%s/src/%s", g_config.repo, repo_headers.items[i]);
      if (!strlist_add(headers, path))
        break;
    }
  }
  strlist_free(&repo_headers);

  // Playground tool sources + headers
  StrList opt_sources = {0};
  if (list_dir_files("opt_src", ".c", &opt_sources)) {
    qsort(opt_sources.items, opt_sources.count, sizeof(char*), strlist_cmp);
    for (size_t i = 0; i < opt_sources.count; i++) {
      char path[1400];
      snprintf(path, sizeof(path), "opt_src/%s", opt_sources.items[i]);
      if (!strlist_add(sources, path))
        break;
    }
  }
  strlist_free(&opt_sources);

  StrList opt_headers = {0};
  if (list_dir_files("opt_src", ".h", &opt_headers)) {
    qsort(opt_headers.items, opt_headers.count, sizeof(char*), strlist_cmp);
    for (size_t i = 0; i < opt_headers.count; i++) {
      char path[1400];
      snprintf(path, sizeof(path), "opt_src/%s", opt_headers.items[i]);
      if (!strlist_add(headers, path))
        break;
    }
  }
  strlist_free(&opt_headers);

  return true;
}

/**
 * @brief Returns whether the binary is missing or older than any input source
 */
static bool needs_build(int flavor, bool* binary_ok) {
  char binary[1200];
  binary_path(flavor, binary, sizeof(binary));

  int64_t binary_time = file_mtime(binary);
  *binary_ok = binary_time >= 0;
  if (binary_time < 0)
    return true;  // Missing -> must build

  StrList sources = {0};
  StrList headers = {0};
  if (!collect_build_inputs(&sources, &headers)) {
    strlist_free(&sources);
    strlist_free(&headers);
    return false;  // No repository: nothing to rebuild against
  }

  bool stale = false;
  for (size_t i = 0; i < sources.count && !stale; i++)
    if (file_mtime(sources.items[i]) > binary_time)
      stale = true;
  for (size_t i = 0; i < headers.count && !stale; i++)
    if (file_mtime(headers.items[i]) > binary_time)
      stale = true;

  strlist_free(&sources);
  strlist_free(&headers);
  return stale;
}

/**
 * @brief Compiles one source file into its object file
 */
static bool compile_source(int flavor, const char* source, Buf* log) {
  // Object name: opt_src files are prefixed to avoid any basename collisions
  const char* base = strrchr(source, '/');
  base = base == NULL ? source : base + 1;
  bool from_opt_src = strncmp(source, "opt_src/", 8) == 0;
  char stem[256];
  snprintf(stem, sizeof(stem), "%.*s", (int)strcspn(base, "."), base);

  char object[1200];
  snprintf(object, sizeof(object), "build/%s/obj/%s%s.o", FLAVOR_NAMES[flavor],
           from_opt_src ? "opt_" : "", stem);

  Buf command = {0};
  buf_printf(&command, "clang -std=c17 -Wall -Wextra -pedantic %s -I\"%s/src\" "
                       "-I\"%s/opt_src\" -c \"%s\" -o \"%s\"",
             flavor == 0 ? "-O3" : "-g -O0", g_config.repo, g_root, source,
             object);

  ProcResult proc = {0};
  bool spawned = run_process(command.data, NULL, 0, &proc);
  buf_free(&command);

  bool ok = spawned && proc.exit_code == 0;
  if (!ok || proc.err.len > 0) {
    buf_printf(log, "> cc %s\n", source);
    if (proc.err.len > 0)
      buf_append(log, proc.err.data, proc.err.len);
    if (!ok)
      buf_printf(log, "(clang exited with %d)\n",
                 spawned ? proc.exit_code : -1);
  }

  proc_result_free(&proc);
  return ok;
}

/**
 * @brief Builds ifj26-opt for the flavor (force skips the staleness check)
 *
 * @param[in] flavor 0 = release, 1 = debug
 * @param[in] force Rebuild even when the binary is fresh
 * @param[out] log Compiler output (may be empty on success)
 * @return Whether the binary is ready afterwards
 */
static bool build_flavor(int flavor, bool force, Buf* log) {
  bool binary_ok = false;
  if (!force && !needs_build(flavor, &binary_ok) && binary_ok)
    return true;  // Fresh, nothing to do

  if (!repo_ok()) {
    buf_printf(log, "repository not found: %s\n", g_config.repo);
    return false;
  }

  double started = now_ms();
  buf_printf(log, "building ifj26-opt (%s)...\n", FLAVOR_NAMES[flavor]);

  char obj_dir[128];
  snprintf(obj_dir, sizeof(obj_dir), "build/%s/obj", FLAVOR_NAMES[flavor]);
  if (!mkdir_p(obj_dir)) {
    buf_printf(log, "failed to create %s\n", obj_dir);
    return false;
  }

  StrList sources = {0};
  StrList headers = {0};
  if (!collect_build_inputs(&sources, &headers)) {
    buf_printf(log, "failed to collect sources from %s\n", g_config.repo);
    strlist_free(&sources);
    strlist_free(&headers);
    return false;
  }

  if (sources.count == 0) {
    buf_puts(log, "no source files found\n");
    strlist_free(&sources);
    strlist_free(&headers);
    return false;
  }

  // Compile every source; stop at the first failure
  bool ok = true;
  for (size_t i = 0; i < sources.count && ok; i++)
    ok = compile_source(flavor, sources.items[i], log);

  // Link
  if (ok) {
    Buf command = {0};
    buf_printf(&command, "clang");
    for (size_t i = 0; i < sources.count; i++) {
      const char* base = strrchr(sources.items[i], '/');
      base = base == NULL ? sources.items[i] : base + 1;
      bool from_opt_src = strncmp(sources.items[i], "opt_src/", 8) == 0;
      char stem[256];
      snprintf(stem, sizeof(stem), "%.*s", (int)strcspn(base, "."), base);
      buf_printf(&command, " \"build/%s/obj/%s%s.o\"", FLAVOR_NAMES[flavor],
                 from_opt_src ? "opt_" : "", stem);
    }
#ifdef _WIN32
    buf_printf(&command, " -o \"build\\%s\\ifj26-opt%s\"", FLAVOR_NAMES[flavor],
               EXE_SUFFIX);
#else
    buf_printf(&command, " -o \"build/%s/ifj26-opt%s\" -lm",
               FLAVOR_NAMES[flavor], EXE_SUFFIX);
#endif

    ProcResult proc = {0};
    if (!run_process(command.data, NULL, 0, &proc)) {
      buf_puts(log, "failed to spawn clang for linking\n");
      ok = false;
    } else if (proc.exit_code != 0) {
      buf_puts(log, "> link\n");
      if (proc.err.len > 0)
        buf_append(log, proc.err.data, proc.err.len);
      buf_printf(log, "(clang exited with %d)\n", proc.exit_code);
      ok = false;
    } else if (proc.err.len > 0) {
      buf_puts(log, "> link\n");
      buf_append(log, proc.err.data, proc.err.len);
    }
    buf_free(&command);
    proc_result_free(&proc);
  }

  strlist_free(&sources);
  strlist_free(&headers);

  double ms = now_ms() - started;
  if (ok) {
    buf_printf(log, "build ok in %.0f ms\n", ms);
    char binary[1200];
    binary_path(flavor, binary, sizeof(binary));
    log_line("built %s in %.0f ms", binary, ms);
  } else {
    buf_printf(log, "build failed after %.0f ms\n", ms);
    log_line("build of %s flavor failed", FLAVOR_NAMES[flavor]);
  }

  if (log->len > MAX_LOG_SIZE) {
    // Keep the tail, which contains the last error
    memmove(log->data, log->data + log->len - MAX_LOG_SIZE, MAX_LOG_SIZE);
    log->len = MAX_LOG_SIZE;
    log->data[log->len] = '\0';
  }

  g_last_build[flavor] = (LastBuild){.valid = true, .ok = ok, .ms = ms};
  return ok;
}

////////////////////////////////////////////////////////////////////////////////
// HTTP plumbing
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Parsed HTTP request
 */
typedef struct {
  char method[8];
  char path[512];
  char query[512];
  char* body;  // malloc'd, content_length bytes + NUL
  size_t content_length;
} Request;

/**
 * @brief Releases the request's body
 */
static void request_free(Request* request) {
  free(request->body);
  request->body = NULL;
}

/**
 * @brief Sends a full HTTP response and logs it
 */
static void send_response(socket_t sock, int status, const char* content_type,
                          const char* body, size_t body_len) {
  const char* status_text =
      status == 200   ? "OK"
      : status == 400 ? "Bad Request"
      : status == 404 ? "Not Found"
      : status == 405 ? "Method Not Allowed"
      : status == 413 ? "Payload Too Large"
                      : "Internal Server Error";

  char header[512];
  int header_len = snprintf(header, sizeof(header),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            status, status_text, content_type, body_len);

  size_t sent_total = 0;
  bool ok = true;
  while (ok && sent_total < (size_t)header_len) {
    int sent = send(sock, header + sent_total, (int)(header_len - sent_total), 0);
    if (sent <= 0)
      ok = false;
    else
      sent_total += (size_t)sent;
  }
  sent_total = 0;
  while (ok && sent_total < body_len) {
    int sent = send(sock, body + sent_total, (int)(body_len - sent_total), 0);
    if (sent <= 0)
      ok = false;
    else
      sent_total += (size_t)sent;
  }

  (void)ok;
}

/**
 * @brief Sends a JSON response
 */
static void send_json(socket_t sock, int status, Buf* json) {
  send_response(sock, status, "application/json", json->data == NULL ? "{}" : json->data,
                json->len);
}

/**
 * @brief Sends a JSON error response: {"error": "<message>"}
 */
static void send_error_json(socket_t sock, int status, const char* message) {
  Buf json = {0};
  buf_puts(&json, "{\"error\":");
  buf_put_json_string(&json, message);
  buf_puts(&json, "}");
  send_json(sock, status, &json);
  buf_free(&json);
}

/**
 * @brief Reads and parses one HTTP request (headers + fixed-size body)
 */
static bool read_request(socket_t sock, Request* request, char* error,
                         size_t error_size) {
  memset(request, 0, sizeof(*request));

  Buf raw = {0};
  bool ok = false;
  size_t header_end = 0;

  // Read until the end of the headers
  while (!ok) {
    if (raw.len > MAX_HEADER_SIZE) {
      snprintf(error, error_size, "request headers too large");
      goto cleanup;
    }

    char chunk[4096];
    int received = recv(sock, chunk, sizeof(chunk), 0);
    if (received <= 0) {
      snprintf(error, error_size, "connection closed while reading headers");
      goto cleanup;
    }
    if (!buf_append(&raw, chunk, (size_t)received)) {
      snprintf(error, error_size, "out of memory");
      goto cleanup;
    }

    const char* end = strstr(raw.data, "\r\n\r\n");
    if (end != NULL) {
      header_end = (size_t)(end - raw.data) + 4;
      ok = true;
    }
  }

  // Parse the request line: METHOD SP TARGET SP HTTP/x.y
  char* line_end = strstr(raw.data, "\r\n");
  *line_end = '\0';

  char* space = strchr(raw.data, ' ');
  if (space == NULL) {
    snprintf(error, error_size, "malformed request line");
    goto cleanup;
  }
  size_t method_len = (size_t)(space - raw.data);
  if (method_len >= sizeof(request->method)) {
    snprintf(error, error_size, "method too long");
    goto cleanup;
  }
  memcpy(request->method, raw.data, method_len);
  request->method[method_len] = '\0';

  char* target = space + 1;
  space = strchr(target, ' ');
  if (space == NULL) {
    snprintf(error, error_size, "malformed request line");
    goto cleanup;
  }
  *space = '\0';

  char* question = strchr(target, '?');
  if (question != NULL) {
    *question = '\0';
    snprintf(request->query, sizeof(request->query), "%s", question + 1);
  }
  if (strlen(target) >= sizeof(request->path)) {
    snprintf(error, error_size, "path too long");
    goto cleanup;
  }
  snprintf(request->path, sizeof(request->path), "%s", target);

  // Headers: find Content-Length
  size_t content_length = 0;
  {
    // Start after the request line's CRLF; earlier NUL writes truncated the
    // request line, so derive the position from the line_end pointer
    char* cursor = line_end + 2;
    while (cursor < raw.data + header_end) {
      char* next = strstr(cursor, "\r\n");
      if (next == NULL)
        break;
      *next = '\0';
      if (strncasecmp(cursor, "content-length:", 15) == 0)
        content_length = (size_t)strtoull(cursor + 15, NULL, 10);
      cursor = next + 2;
    }
  }

  if (content_length > MAX_BODY_SIZE) {
    snprintf(error, error_size, "request body too large");
    goto cleanup;
  }

  // Copy whatever body bytes already arrived, then read the rest
  request->body = malloc(content_length + 1);
  if (request->body == NULL) {
    snprintf(error, error_size, "out of memory");
    goto cleanup;
  }

  size_t have = raw.len - header_end;
  if (have > content_length)
    have = content_length;
  memcpy(request->body, raw.data + header_end, have);
  request->content_length = content_length;

  while (have < content_length) {
    int received = recv(sock, request->body + have, (int)(content_length - have), 0);
    if (received <= 0) {
      snprintf(error, error_size, "connection closed while reading body");
      goto cleanup;
    }
    have += (size_t)received;
  }
  request->body[content_length] = '\0';

  ok = true;

cleanup:
  buf_free(&raw);
  return ok;
}

/**
 * @brief Parses the flavor query parameter (default release)
 *
 * @return 0 = release, 1 = debug, -1 = invalid
 */
static int parse_flavor(const char* query) {
  const char* flavor = strstr(query, "flavor=");
  if (flavor == NULL)
    return 0;  // Default
  flavor += 7;

  if (strncmp(flavor, "release", 7) == 0)
    return 0;
  if (strncmp(flavor, "debug", 5) == 0)
    return 1;
  return -1;
}

////////////////////////////////////////////////////////////////////////////////
// Endpoint handlers
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief GET /api/status: repository + build state
 */
static void handle_status(socket_t sock) {
  Buf json = {0};
  bool repo_found = repo_ok();

  buf_printf(&json, "{\"repo\":");
  buf_put_json_string(&json, g_config.repo);
  buf_printf(&json, ",\"repo_ok\":%s,\"flavors\":{", repo_found ? "true" : "false");

  for (int flavor = 0; flavor < FLAVOR_COUNT; flavor++) {
    bool binary_ok = false;
    bool stale = repo_found ? needs_build(flavor, &binary_ok) : false;

    buf_printf(&json, "%s\"%s\":{\"binary_ok\":%s,\"stale\":%s,\"last_build\":",
               flavor == 0 ? "" : ",", FLAVOR_NAMES[flavor],
               binary_ok ? "true" : "false", stale ? "true" : "false");
    if (g_last_build[flavor].valid)
      buf_printf(&json, "{\"ok\":%s,\"ms\":%.0f}",
                 g_last_build[flavor].ok ? "true" : "false", g_last_build[flavor].ms);
    else
      buf_puts(&json, "null");
    buf_puts(&json, "}");
  }

  buf_puts(&json, "}}");
  send_json(sock, 200, &json);
  buf_free(&json);
}

/**
 * @brief GET /api/presets: sorted preset names (without the .ir suffix)
 */
static void handle_presets(socket_t sock) {
  StrList names = {0};
  Buf json = {0};

  if (list_dir_files("presets", ".ir", &names)) {
    qsort(names.items, names.count, sizeof(char*), strlist_cmp);

    buf_puts(&json, "[");
    for (size_t i = 0; i < names.count; i++) {
      const char* name = names.items[i];
      size_t len = strlen(name);
      if (len > 3)
        len -= 3;  // Strip ".ir"

      if (i > 0)
        buf_puts(&json, ",");
      buf_printf(&json, "\"%.*s\"", (int)len, name);
    }
    buf_puts(&json, "]");
  } else {
    buf_puts(&json, "[]");
  }

  send_json(sock, 200, &json);
  buf_free(&json);
  strlist_free(&names);
}

/**
 * @brief POST /api/optimize: build if stale, then run ifj26-opt on the body
 */
static void handle_optimize(socket_t sock, const char* query, const char* body,
                            size_t body_len) {
  int flavor = parse_flavor(query);
  if (flavor < 0) {
    send_error_json(sock, 400, "invalid flavor (use release or debug)");
    return;
  }

  if (!repo_ok()) {
    Buf json = {0};
    buf_printf(&json, "{\"build\":{\"performed\":false,\"ok\":false,\"ms\":0,\"log\":");
    buf_put_json_string(&json, "");
    buf_printf(&json, "},\"result\":null,\"error\":");
    buf_put_json_string(&json, "repository not found (check config.txt)");
    buf_puts(&json, "}");
    send_json(sock, 200, &json);
    buf_free(&json);
    return;
  }

  // Hot reload: rebuild first when any input source is newer than the binary
  Buf build_log = {0};
  bool performed = false;
  bool build_ok = true;
  double build_ms = 0;
  bool binary_ok = false;
  if (needs_build(flavor, &binary_ok)) {
    performed = true;
    build_ok = build_flavor(flavor, false, &build_log);
    binary_ok = build_ok;  // build_flavor returns true only when ready
    build_ms = g_last_build[flavor].valid ? g_last_build[flavor].ms : 0;
  }

  Buf response = {0};
  if (!build_ok || !binary_ok) {
    buf_printf(&response, "{\"build\":{\"performed\":%s,\"ok\":false,\"ms\":%.0f,\"log\":",
               performed ? "true" : "false", build_ms);
    buf_put_json_string(&response, build_log.data);
    buf_puts(&response, "},\"result\":null}");
    send_json(sock, 200, &response);
    buf_free(&response);
    buf_free(&build_log);
    return;
  }

  // Spawn the freshly built tool (a new process per request -> always current)
  char binary[1200];
  binary_path(flavor, binary, sizeof(binary));
  char command[1400];
  snprintf(command, sizeof(command), "\"%s\"", binary);

  ProcResult proc = {0};
  if (!run_process(command, body, body_len, &proc)) {
    send_error_json(sock, 500, "failed to spawn ifj26-opt");
    proc_result_free(&proc);
    buf_free(&response);
    buf_free(&build_log);
    return;
  }

  buf_printf(&response, "{\"build\":{\"performed\":%s,\"ok\":true,\"ms\":%.0f,\"log\":",
             performed ? "true" : "false", build_ms);
  buf_put_json_string(&response, build_log.data);
  buf_puts(&response, "},\"stderr\":");
  buf_put_json_string(&response, proc.err.data != NULL ? proc.err.data : "");

  if (proc.out.len > 0) {
    // The tool's stdout is a complete JSON envelope; embed it verbatim
    buf_puts(&response, ",\"result\":");
    buf_append(&response, proc.out.data, proc.out.len);
  } else {
    buf_printf(&response,
               ",\"result\":{\"ok\":false,\"error\":\"ifj26-opt exited with code %d "
               "without output\"}",
               proc.exit_code);
  }
  buf_puts(&response, "}");

  send_json(sock, 200, &response);
  buf_free(&response);
  buf_free(&build_log);
  proc_result_free(&proc);
}

/**
 * @brief POST /api/rebuild: force a rebuild of the flavor
 */
static void handle_rebuild(socket_t sock, const char* query) {
  int flavor = parse_flavor(query);
  if (flavor < 0) {
    send_error_json(sock, 400, "invalid flavor (use release or debug)");
    return;
  }

  Buf log = {0};
  bool ok = build_flavor(flavor, true, &log);

  Buf json = {0};
  buf_printf(&json, "{\"ok\":%s,\"ms\":%.0f,\"log\":", ok ? "true" : "false",
             g_last_build[flavor].valid ? g_last_build[flavor].ms : 0);
  buf_put_json_string(&json, log.data);
  buf_puts(&json, "}");
  send_json(sock, 200, &json);
  buf_free(&json);
  buf_free(&log);
}

/**
 * @brief Sends a static asset (whitelisted paths only)
 */
static void serve_static(socket_t sock, const char* relative_path) {
  Buf content = {0};
  if (!read_whole_file(relative_path, &content)) {
    buf_free(&content);
    send_error_json(sock, 404, "not found");
    return;
  }

  const char* type = "text/plain; charset=utf-8";
  const char* dot = strrchr(relative_path, '.');
  if (dot != NULL) {
    if (strcmp(dot, ".html") == 0)
      type = "text/html; charset=utf-8";
    else if (strcmp(dot, ".js") == 0)
      type = "text/javascript; charset=utf-8";
    else if (strcmp(dot, ".css") == 0)
      type = "text/css; charset=utf-8";
  }

  send_response(sock, 200, type, content.data != NULL ? content.data : "",
                content.len);
  buf_free(&content);
}

/**
 * @brief Serves a preset file after validating the name
 */
static void serve_preset(socket_t sock, const char* name) {
  // Names must be plain file names; reject anything path-like
  size_t len = strlen(name);
  if (len == 0 || len > 128) {
    send_error_json(sock, 404, "not found");
    return;
  }
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) {
      send_error_json(sock, 404, "not found");
      return;
    }
  }
  if (strstr(name, "..") != NULL) {
    send_error_json(sock, 404, "not found");
    return;
  }

  char path[1400];
  snprintf(path, sizeof(path), "presets/%s.ir", name);
  serve_static(sock, path);
}

/**
 * @brief Routes one handled client connection
 */
static void handle_client(socket_t sock) {
  Request request;
  char error[128];
  if (!read_request(sock, &request, error, sizeof(error))) {
    send_error_json(sock, 400, error);
    return;
  }

  bool is_get = strcmp(request.method, "GET") == 0;
  bool is_post = strcmp(request.method, "POST") == 0;
  const char* path = request.path;

  if (strcmp(path, "/api/optimize") == 0) {
    if (is_post)
      handle_optimize(sock, request.query, request.body, request.content_length);
    else
      send_error_json(sock, 405, "use POST");
  } else if (strcmp(path, "/api/rebuild") == 0) {
    if (is_post)
      handle_rebuild(sock, request.query);
    else
      send_error_json(sock, 405, "use POST");
  } else if (strcmp(path, "/api/status") == 0) {
    if (is_get)
      handle_status(sock);
    else
      send_error_json(sock, 405, "use GET");
  } else if (strcmp(path, "/api/presets") == 0) {
    if (is_get)
      handle_presets(sock);
    else
      send_error_json(sock, 405, "use GET");
  } else if (is_get && strncmp(path, "/presets/", 9) == 0) {
    serve_preset(sock, path + 9);
  } else if (is_get && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
    serve_static(sock, "static/index.html");
  } else if (is_get && strcmp(path, "/app.js") == 0) {
    serve_static(sock, "static/app.js");
  } else if (is_get && strcmp(path, "/style.css") == 0) {
    serve_static(sock, "static/style.css");
  } else {
    send_error_json(sock, 404, "not found");
  }

  log_line("%s %s", request.method, path);
  request_free(&request);
}

////////////////////////////////////////////////////////////////////////////////
// Entrypoint
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Server entrypoint
 */
int main(int argc, char** argv) {
  // CLI flags (override config.txt)
  bool build_only = false;
  const char* repo_override = NULL;
  int port_override = -1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--build-only") == 0) {
      build_only = true;
    } else if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
      repo_override = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port_override = atoi(argv[++i]);
    } else {
      fprintf(stderr,
              "Usage: server [--build-only] [--repo <path>] [--port <n>]\n");
      return 2;
    }
  }

  load_config("config.txt");
  if (repo_override != NULL) {
    size_t len = strlen(repo_override);
    if (len < sizeof(g_config.repo))
      memcpy(g_config.repo, repo_override, len + 1);
  }
  if (port_override > 0)
    g_config.port = port_override;

  // Remember the playground root (current directory) for absolute includes
#ifdef _WIN32
  DWORD root_len = GetCurrentDirectoryA(sizeof(g_root), g_root);
  if (root_len == 0)
    g_root[0] = '\0';
#else
  if (getcwd(g_root, sizeof(g_root)) == NULL)
    g_root[0] = '\0';
#endif

  if (build_only) {
    int rc = 0;
    for (int flavor = 0; flavor < FLAVOR_COUNT; flavor++) {
      Buf log = {0};
      bool ok = build_flavor(flavor, false, &log);
      printf("== %s ==\n%s\n", FLAVOR_NAMES[flavor],
             log.data != NULL ? log.data : "");
      buf_free(&log);
      if (!ok)
        rc = 1;
    }
    return rc;
  }

  if (!repo_ok())
    fprintf(stderr,
            "warning: repository not found at '%s' - set 'repo' in config.txt\n",
            g_config.repo);

#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "WSAStartup failed\n");
    return 1;
  }
#endif

  socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener == SOCKET_INVALID) {
    fprintf(stderr, "socket() failed\n");
    return 1;
  }

  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse,
             sizeof(reuse));

  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((unsigned short)g_config.port);

  if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0) {
    fprintf(stderr, "bind() failed on port %d (already in use?)\n", g_config.port);
    return 1;
  }
  if (listen(listener, 8) != 0) {
    fprintf(stderr, "listen() failed\n");
    return 1;
  }

  log_line("IFJ26 Optimizer Debugger -> http://127.0.0.1:%d", g_config.port);
  log_line("repo: %s", g_config.repo);
  log_line("press Ctrl+C to stop");

  for (;;) {
    socket_t client = accept(listener, NULL, NULL);
    if (client == SOCKET_INVALID)
      continue;

    // Do not let a stalled client wedge the single-threaded server
#ifdef _WIN32
    DWORD timeout_ms = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms,
               sizeof(timeout_ms));
#else
    struct timeval timeout = {10, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout,
               sizeof(timeout));
#endif

    handle_client(client);
    CLOSE_SOCKET(client);
  }

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
