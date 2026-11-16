/* Compile like server.c, then run: build/process-timeouts.exe */
#define main debugger_server_main
#include "../server.c"
#undef main
#include <assert.h>

int main(int argc, char** argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "--ok") == 0) {
      fputs("ok", stdout);
      return 0;
    }
    if (strcmp(argv[1], "--tree") == 0) {
#ifdef _WIN32
      char command[4096];
      snprintf(command, sizeof(command), "\"%s\" --hang", argv[0]);
      STARTUPINFOA startup = {0};
      startup.cb = sizeof(startup);
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      PROCESS_INFORMATION child = {0};
      assert(CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                            NULL, NULL, &startup, &child));
      CloseHandle(child.hThread);
      CloseHandle(child.hProcess);
      return 0;
#else
      pid_t child = fork();
      assert(child >= 0);
      if (child != 0) return 0;
#endif
    }
    fputs("partial output", stdout);
    fflush(stdout);
    for (;;) {
#ifdef _WIN32
      Sleep(1000);
#else
      sleep(1);
#endif
    }
  }
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif
  char command[4096];
  snprintf(command, sizeof(command), "\"%s\" --hang", argv[0]);
  char input[256 * 1024] = {0};
  // Check both blocked output reads and a blocked write to unconsumed stdin.
  for (int blocked_input = 0; blocked_input < 2; blocked_input++) {
    ProcResult result = {0};
    double start = now_ms();
    assert(run_process(command, blocked_input ? input : NULL,
                       blocked_input ? sizeof(input) : 0, 200, &result));
    assert(result.timed_out && result.exit_code == 124);
    assert(now_ms() - start < 5000);
    assert(result.out.data && strstr(result.out.data, "partial output"));
    assert(result.err.data && strstr(result.err.data, "timed out"));
    proc_result_free(&result);
  }
  // A child that exits can leave descendants holding the output pipes open.
  snprintf(command, sizeof(command), "\"%s\" --tree", argv[0]);
  ProcResult tree = {0};
  double start = now_ms();
  assert(run_process(command, NULL, 0, 500, &tree));
  assert(tree.timed_out && tree.exit_code == 124);
  assert(now_ms() - start < 5000);
  proc_result_free(&tree);
  // A timeout must leave the runner usable for the next request.
  snprintf(command, sizeof(command), "\"%s\" --ok", argv[0]);
  ProcResult result = {0};
  assert(run_process(command, NULL, 0, 2000, &result));
  assert(!result.timed_out && result.exit_code == 0);
  assert(result.out.data && strcmp(result.out.data, "ok") == 0);
  proc_result_free(&result);
  puts("PASS subprocess timeouts, blocked stdin, partial output, descendants, and recovery");
  return 0;
}
