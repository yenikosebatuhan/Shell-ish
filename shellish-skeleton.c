#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h> // termios, TCSANOW, ECHO, ICANON
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>

const char *sysname = "shellish";

enum return_codes {
  SUCCESS = 0,
  EXIT = 1,
  UNKNOWN = 2,
};

struct command_t {
  char *name;
  bool background;
  bool auto_complete;
  int arg_count;
  char **args;
  char *redirects[3];     // in/out redirection
  struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command) {
  int i = 0;
  printf("Command: <%s>\n", command->name);
  printf("\tIs Background: %s\n", command->background ? "yes" : "no");
  printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
  printf("\tRedirects:\n");
  for (i = 0; i < 3; i++)
    printf("\t\t%d: %s\n", i,
           command->redirects[i] ? command->redirects[i] : "N/A");
  printf("\tArguments (%d):\n", command->arg_count);
  for (i = 0; i < command->arg_count; ++i)
    printf("\t\tArg %d: %s\n", i, command->args[i]);
  if (command->next) {
    printf("\tPiped to:\n");
    print_command(command->next);
  }
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command) {
  if (command->arg_count) {
    for (int i = 0; i < command->arg_count; ++i)
      free(command->args[i]);
    free(command->args);
  }
  for (int i = 0; i < 3; ++i)
    if (command->redirects[i])
      free(command->redirects[i]);
  if (command->next) {
    free_command(command->next);
    command->next = NULL;
  }
  free(command->name);
  free(command);
  return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt() {
  char cwd[1024], hostname[1024];
  gethostname(hostname, sizeof(hostname));
  getcwd(cwd, sizeof(cwd));
  printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
  return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command) {
  const char *splitters = " \t"; // split at whitespace
  int index, len;
  len = strlen(buf);
  while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
  {
    buf++;
    len--;
  }
  while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
    buf[--len] = 0; // trim right whitespace

  if (len > 0 && buf[len - 1] == '?') // auto-complete
    command->auto_complete = true;
  if (len > 0 && buf[len - 1] == '&') // background
    command->background = true;

  char *pch = strtok(buf, splitters);
  if (pch == NULL) {
    command->name = (char *)malloc(1);
    command->name[0] = 0;
  } else {
    command->name = (char *)malloc(strlen(pch) + 1);
    strcpy(command->name, pch);
  }

  command->args = (char **)malloc(sizeof(char *));

  int redirect_index;
  int arg_index = 0;
  char temp_buf[1024], *arg;
  while (1) {
    // tokenize input on splitters
    pch = strtok(NULL, splitters);
    if (!pch)
      break;
    arg = temp_buf;
    strcpy(arg, pch);
    len = strlen(arg);

    if (len == 0)
      continue; // empty arg, go for next
    while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
    {
      arg++;
      len--;
    }
    while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
      arg[--len] = 0; // trim right whitespace
    if (len == 0)
      continue; // empty arg, go for next

    // piping to another command
    if (strcmp(arg, "|") == 0) {
      struct command_t *c =
          (struct command_t *)malloc(sizeof(struct command_t));
      int l = strlen(pch);
      pch[l] = splitters[0]; // restore strtok termination
      index = 1;
      while (pch[index] == ' ' || pch[index] == '\t')
        index++; // skip whitespaces

      parse_command(pch + index, c);
      pch[l] = 0; // put back strtok termination
      command->next = c;
      continue;
    }

    // background process
    if (strcmp(arg, "&") == 0)
      continue; // handled before

    // handle input redirection
    redirect_index = -1;
    if (arg[0] == '<')
      redirect_index = 0;
    if (arg[0] == '>') {
      if (len > 1 && arg[1] == '>') {
        redirect_index = 2;
        arg++;
        len--;
      } else
        redirect_index = 1;
    }
    if (redirect_index != -1) {
      command->redirects[redirect_index] = (char *)malloc(len);
      strcpy(command->redirects[redirect_index], arg + 1);
      continue;
    }

    // normal arguments
    if (len > 2 &&
        ((arg[0] == '"' && arg[len - 1] == '"') ||
         (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
    {
      arg[--len] = 0;
      arg++;
    }
    command->args =
        (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
    command->args[arg_index] = (char *)malloc(len + 1);
    strcpy(command->args[arg_index++], arg);
  }
  command->arg_count = arg_index;

  // increase args size by 2
  command->args = (char **)realloc(command->args,
                                   sizeof(char *) * (command->arg_count += 2));

  // shift everything forward by 1
  for (int i = command->arg_count - 2; i > 0; --i)
    command->args[i] = command->args[i - 1];

  // set args[0] as a copy of name
  command->args[0] = strdup(command->name);
  // set args[arg_count-1] (last) to NULL
  command->args[command->arg_count - 1] = NULL;

  return 0;
}

void prompt_backspace() {
  putchar(8);   // go back 1
  putchar(' '); // write empty over
  putchar(8);   // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command) {
  int index = 0;
  char c;
  char buf[4096];
  static char oldbuf[4096];

  static struct termios backup_termios, new_termios;
  tcgetattr(STDIN_FILENO, &backup_termios);
  new_termios = backup_termios;
  new_termios.c_lflag &=
      ~(ICANON |
        ECHO); // Also disable automatic echo. We manually echo each char.
  tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

  show_prompt();
  buf[0] = 0;
  while (1) {
    c = getchar();

    if (c == 9) // handle tab
    {
      buf[index++] = '?'; // autocomplete
      break;
    }

    if (c == 127) // handle backspace
    {
      if (index > 0) {
        prompt_backspace();
        index--;
      }
      continue;
    }

    if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
      continue;
    }

    if (c == 65) // up arrow
    {
      while (index > 0) {
        prompt_backspace();
        index--;
      }

      char tmpbuf[4096];
      printf("%s", oldbuf);
      strcpy(tmpbuf, buf);
      strcpy(buf, oldbuf);
      strcpy(oldbuf, tmpbuf);
      index += strlen(buf);
      continue;
    }

    putchar(c); // echo the character
    buf[index++] = c;
    if (index >= sizeof(buf) - 1)
      break;
    if (c == '\n') // enter key
      break;
    if (c == 4) // Ctrl+D
      return EXIT;
  }
  if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
    index--;
  buf[index++] = '\0'; // null terminate string

  strcpy(oldbuf, buf);

  parse_command(buf, command);

  tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  return SUCCESS;
}

static char *resolve_path(const char *cmd) {

  if (strchr(cmd, '/') != NULL) {
    return strdup(cmd);
  }

  char *path_env = getenv("PATH");
  if (path_env == NULL)
    return NULL;

  char *path_copy = strdup(path_env);
  if (path_copy == NULL)
    return NULL;

  char *saveptr = NULL;
  char *dir = strtok_r(path_copy, ":", &saveptr);

  char candidate[PATH_MAX];

  while (dir != NULL) {
    snprintf(candidate, sizeof(candidate), "%s/%s", dir, cmd);

    if (access(candidate, X_OK) == 0) {

      free(path_copy);
      return strdup(candidate);
    }

    dir = strtok_r(NULL, ":", &saveptr);
  }

  free(path_copy);
  return NULL;

}

static void apply_redirects(struct command_t *command) {
  if (command->redirects[0] != NULL) {
    int fd_in = open(command->redirects[0], O_RDONLY);
    if (fd_in < 0) {
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->redirects[0], strerror(errno));
      exit(1);
    }
    if (dup2(fd_in, STDIN_FILENO) < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      close(fd_in);
      exit(1);
    }
    close(fd_in);
  }

  if (command->redirects[2] != NULL) {
    int fd_out = open(command->redirects[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_out < 0) {
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->redirects[2], strerror(errno));
      exit(1);
    }
    if (dup2(fd_out, STDOUT_FILENO) < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      close(fd_out);
      exit(1);
    }
    close(fd_out);
  } else if (command->redirects[1] != NULL) {
    int fd_out = open(command->redirects[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->redirects[1], strerror(errno));
      exit(1);
    }
    if (dup2(fd_out, STDOUT_FILENO) < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      close(fd_out);
      exit(1);
    }
    close(fd_out);
  }
}

static int run_pipeline(struct command_t *command) {
  int prev_read = -1;
  pid_t pids[256];
  int pid_count = 0;

  struct command_t *cur = command;

  while (cur != NULL) {
    int pipefd[2] = {-1, -1};
    if (cur->next != NULL) {
      if (pipe(pipefd) < 0) {
        fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
        return SUCCESS;
      }
    }

    pid_t pid = fork();
    if (pid == 0) {
      if (prev_read != -1) {
        dup2(prev_read, STDIN_FILENO);
      }
      if (cur->next != NULL) {
        dup2(pipefd[1], STDOUT_FILENO);
      }

      if (prev_read != -1) close(prev_read);
      if (pipefd[0] != -1) close(pipefd[0]);
      if (pipefd[1] != -1) close(pipefd[1]);

      apply_redirects(cur);

      char *full_path = resolve_path(cur->name);
      if (full_path != NULL) {
        execv(full_path, cur->args);
        fprintf(stderr, "-%s: %s: %s\n", sysname, cur->name, strerror(errno));
        free(full_path);
        exit(126);
      } else {
        fprintf(stderr, "-%s: %s: command not found\n", sysname, cur->name);
        exit(127);
      }
    }

    if (pid < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      if (prev_read != -1) close(prev_read);
      if (pipefd[0] != -1) close(pipefd[0]);
      if (pipefd[1] != -1) close(pipefd[1]);
      return SUCCESS;
    }

    if (pid_count < 256) {
      pids[pid_count++] = pid;
    }

    if (prev_read != -1) close(prev_read);
    if (pipefd[1] != -1) close(pipefd[1]);
    prev_read = pipefd[0];

    cur = cur->next;
  }

  if (prev_read != -1) close(prev_read);

  if (command->background) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    return SUCCESS;
  } else {
    for (int i = 0; i < pid_count; i++) {
      waitpid(pids[i], NULL, 0);
    }
    return SUCCESS;
  }
}

int process_command(struct command_t *command) {
  int r;
  if (strcmp(command->name, "") == 0)
    return SUCCESS;

  if (strcmp(command->name, "exit") == 0)
    return EXIT;

  if (strcmp(command->name, "cd") == 0) {
    if (command->arg_count > 0) {
      r = chdir(command->args[1]);
      if (r == -1)
        printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
      return SUCCESS;
    }
  }

  if (command->next != NULL) {
    return run_pipeline(command);
  }

  pid_t pid = fork();
  if (pid == 0) // child
  {
    // TODO: do your own exec with path resolving using execv()
    // do so by replacing the execvp call below
    apply_redirects(command);

    char *full_path = resolve_path(command->name);

    if (full_path != NULL) {
      execv(full_path, command->args);
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->name, strerror(errno));
      free(full_path);
      exit(126);
    } else {
      // resolve_path returned NULL
      fprintf(stderr, "-%s: %s: command not found\n", sysname, command->name);
      exit(127); // 127 is standard for "Command not found"
    }
  } else {
    // TODO: implement background processes here
    if (command->background) {

      while (waitpid(-1, NULL, WNOHANG) > 0) {}
      return SUCCESS;
    } else {

      waitpid(pid, NULL, 0);
      return SUCCESS;
    }
  }
}

int main() {
  while (1) {
    struct command_t *command =
        (struct command_t *)malloc(sizeof(struct command_t));
    memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

    int code;
    code = prompt(command);
    if (code == EXIT)
      break;

    code = process_command(command);
    if (code == EXIT)
      break;

    free_command(command);
  }

  printf("\n");
  return 0;
}
