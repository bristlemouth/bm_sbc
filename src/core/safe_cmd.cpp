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

  // Copy string to local variable
  size_t cmd_len = strlen(cmd) + 1;
  char *local = static_cast<char *>(malloc(cmd_len));
  if (!local) {
    return -1;
  }
  strncpy(local, cmd, cmd_len);

  char *token = strtok(local, " ");
  while (token) {
    argv.push_back(token);
    token = strtok(NULL, " ");
  }
  argv.push_back(nullptr);

  free(local);

  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
