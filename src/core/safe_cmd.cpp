#include "safe_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

/*!
 @brief Prevent command injection with a safe system call

 @details Uses execvp instead of system to prevent command injection from
          calls that have exposure to the outside world.

 @param cmd command string to run

 @return 0 on success
         other on failure
 */
int safe_cmd(const char *cmd) {
  std::vector<char *> argv;

  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }

  // Copy string to local variable
  size_t cmd_len = strlen(cmd) + 1;
  char *local = static_cast<char *>(malloc(cmd_len));
  if (!local) {
    return -1;
  }
  strncpy(local, cmd, cmd_len);

  char *save = nullptr;
  char *token = strtok_r(local, " ", &save);
  while (token) {
    argv.push_back(token);
    token = strtok_r(NULL, " ", &save);
  }
  argv.push_back(nullptr);

  if (argv.size() == 1) {
    free(local);
    return -1;
  }

  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  int status = 0;
  pid_t r;
  while ((r = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
  }
  free(local);
  if (r < 0) {
    return -1;
  }

  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
