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
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>

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
  char *redirects[3];     // redirects[0] = input (<), redirects[1] = output (>), redirects[2] = append (>>)
  struct command_t *next; // next command in pipe chain (cmd1 | cmd2 | cmd3)
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
  // free argument strings
  if (command->arg_count) {
    for (int i = 0; i < command->arg_count; ++i)
      free(command->args[i]);
    free(command->args);
  }
  // free redirection file names if they exist
  for (int i = 0; i < 3; ++i)
    if (command->redirects[i])
      free(command->redirects[i]);

  // if there is a piped command, free it recursively
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
  // basic shell prompt with user@host:cwd
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

  // remove leading spaces/tabs
  while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
  {
    buf++;
    len--;
  }
  // remove trailing spaces/tabs
  while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
    buf[--len] = 0; // trim right whitespace

  // user can press TAB for autocomplete (we mark it with '?')
  if (len > 0 && buf[len - 1] == '?') // auto-complete
    command->auto_complete = true;

  // '&' means run in background
  if (len > 0 && buf[len - 1] == '&') // background
    command->background = true;

  // first token is the command name
  char *pch = strtok(buf, splitters);
  if (pch == NULL) {
    command->name = (char *)malloc(1);
    command->name[0] = 0;
  } else {
    command->name = (char *)malloc(strlen(pch) + 1);
    strcpy(command->name, pch);
  }

  // start args array (will grow with realloc)
  command->args = (char **)malloc(sizeof(char *));

  int redirect_index;
  int arg_index = 0;
  char temp_buf[1024], *arg;

  while (1) {
    // tokenize remaining parts
    pch = strtok(NULL, splitters);
    if (!pch)
      break;

    arg = temp_buf;
    strcpy(arg, pch);
    len = strlen(arg);

    if (len == 0)
      continue; // empty arg, go for next

    // trim left whitespace
    while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
    {
      arg++;
      len--;
    }
    // trim right whitespace
    while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
      arg[--len] = 0; // trim right whitespace

    if (len == 0)
      continue; // empty arg, go for next

    // '|' means pipe: create next command and parse the rest recursively
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

    // '&' is already handled (background flag)
    if (strcmp(arg, "&") == 0)
      continue; // handled before

    // redirection parsing:
    // <file  -> redirects[0]
    // >file  -> redirects[1]
    // >>file -> redirects[2]
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
      // IMPORTANT: our parser expects no space like >out.txt or <in.txt
      command->redirects[redirect_index] = (char *)malloc(len);
      strcpy(command->redirects[redirect_index], arg + 1);
      continue;
    }

    // normal arguments (also support quoted strings)
    if (len > 2 &&
        ((arg[0] == '"' && arg[len - 1] == '"') ||
         (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
    {
      arg[--len] = 0;
      arg++;
    }

    // add arg to args array
    command->args =
        (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
    command->args[arg_index] = (char *)malloc(len + 1);
    strcpy(command->args[arg_index++], arg);
  }
  command->arg_count = arg_index;

  // we need argv format for execv: argv[0]=name ... argv[last]=NULL
  command->args = (char **)realloc(command->args,
                                   sizeof(char *) * (command->arg_count += 2));

  // shift everything forward by 1 so we can insert argv[0]
  for (int i = command->arg_count - 2; i > 0; --i)
    command->args[i] = command->args[i - 1];

  // set args[0] as the command name
  command->args[0] = strdup(command->name);

  // last arg must be NULL for execv
  command->args[command->arg_count - 1] = NULL;

  return 0;
}

void prompt_backspace() {
  putchar(8);   // move cursor back
  putchar(' '); // erase char
  putchar(8);   // move cursor back again
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

  // we use termios to read char-by-char (no canonical mode)
  static struct termios backup_termios, new_termios;
  tcgetattr(STDIN_FILENO, &backup_termios);
  new_termios = backup_termios;

  // disable ICANON (line buffering) and ECHO (we echo manually)
  new_termios.c_lflag &=
      ~(ICANON |
        ECHO); // Also disable automatic echo. We manually echo each char.
  tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

  show_prompt();
  buf[0] = 0;

  while (1) {
    c = getchar();

    if (c == 9) // TAB key
    {
      buf[index++] = '?'; // mark autocomplete request
      break;
    }

    if (c == 127) // backspace key
    {
      if (index > 0) {
        prompt_backspace();
        index--;
      }
      continue;
    }

    // ignore some escape sequence bytes for arrows
    if (c == 27 || c == 91 || c == 66 || c == 67 || c == 68) {
      continue;
    }

    if (c == 65) // up arrow: show previous command
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

    // echo typed character ourselves
    putchar(c);
    buf[index++] = c;

    if (index >= sizeof(buf) - 1)
      break;

    if (c == '\n') // ENTER
      break;

    if (c == 4) // Ctrl+D -> exit shell
      return EXIT;
  }

  // remove newline at the end
  if (index > 0 && buf[index - 1] == '\n')
    index--;

  // null terminate string
  buf[index++] = '\0';

  // save command for up arrow history
  strcpy(oldbuf, buf);

  // fill command struct from input string
  parse_command(buf, command);

  // restore terminal settings before executing
  tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
  return SUCCESS;
}

// This function tries to find full path of a command using PATH env.
// We need this because we use execv (not execvp).
static char *resolve_path(const char *cmd) {

  // if command already has '/', we treat it as a path
  if (strchr(cmd, '/') != NULL) {
    return strdup(cmd);
  }

  // get PATH (like /bin:/usr/bin:...)
  char *path_env = getenv("PATH");
  if (path_env == NULL)
    return NULL;

  // strtok will modify the string, so we copy it first
  char *path_copy = strdup(path_env);
  if (path_copy == NULL)
    return NULL;

  char *saveptr = NULL;
  char *dir = strtok_r(path_copy, ":", &saveptr);

  char candidate[PATH_MAX];

  // try each directory in PATH
  while (dir != NULL) {
    snprintf(candidate, sizeof(candidate), "%s/%s", dir, cmd);

    // check if file exists and executable
    if (access(candidate, X_OK) == 0) {
      free(path_copy);
      return strdup(candidate);
    }

    dir = strtok_r(NULL, ":", &saveptr);
  }

  free(path_copy);
  return NULL;
}

// Apply <, >, >> redirections by opening files and dup2 to stdin/stdout
static void apply_redirects(struct command_t *command) {

  // input redirection: <file
  if (command->redirects[0] != NULL) {
    int fd_in = open(command->redirects[0], O_RDONLY);
    if (fd_in < 0) {
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->redirects[0], strerror(errno));
      exit(1);
    }
    // replace stdin with file
    if (dup2(fd_in, STDIN_FILENO) < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      close(fd_in);
      exit(1);
    }
    close(fd_in);
  }

  // output append redirection: >>file
  if (command->redirects[2] != NULL) {
    int fd_out = open(command->redirects[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_out < 0) {
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->redirects[2], strerror(errno));
      exit(1);
    }
    // replace stdout with file
    if (dup2(fd_out, STDOUT_FILENO) < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      close(fd_out);
      exit(1);
    }
    close(fd_out);
  } else if (command->redirects[1] != NULL) {
    // output overwrite redirection: >file
    int fd_out = open(command->redirects[1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->redirects[1], strerror(errno));
      exit(1);
    }
    // replace stdout with file
    if (dup2(fd_out, STDOUT_FILENO) < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      close(fd_out);
      exit(1);
    }
    close(fd_out);
  }
}

// simple helper: parse a positive integer, return -1 if not valid
static int parse_positive_int(const char *s) {
  if (s == NULL || *s == '\0') return -1;
  int x = 0;
  for (int i = 0; s[i] != '\0'; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    x = x * 10 + (s[i] - '0');
  }
  return x;
}

// Custom command: pinfo <pid>
// We read /proc/<pid>/status and print some fields.
static int builtin_pinfo(struct command_t *command) {

  // if user didn't give pid
  if (command->args[1] == NULL) {
    fprintf(stderr, "-%s: pinfo: missing pid\n", sysname);
    return SUCCESS;
  }

  // only accept numeric pid
  int pid = parse_positive_int(command->args[1]);
  if (pid <= 0) {
    fprintf(stderr, "-%s: pinfo: invalid pid\n", sysname);
    return SUCCESS;
  }

  // build /proc/<pid>/status path
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/proc/%d/status", pid);

  FILE *f = fopen(path, "r");
  if (!f) {
    // if file does not exist, process probably not found
    fprintf(stderr, "-%s: pinfo: %s\n", sysname, strerror(errno));
    return SUCCESS;
  }

  // read status file and print some important lines
  char line[512];
  int shown = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "Name:", 5) == 0 ||
        strncmp(line, "State:", 6) == 0 ||
        strncmp(line, "PPid:", 5) == 0 ||
        strncmp(line, "VmSize:", 7) == 0 ||
        strncmp(line, "VmRSS:", 6) == 0) {
      fputs(line, stdout);
      shown++;
    }
    if (shown >= 5) break; // stop after printing 5 items
  }
  fclose(f);
  return SUCCESS;
}

// create room directory if it does not exist
static int ensure_dir_exists(const char *dir) {
  struct stat st;
  if (stat(dir, &st) == 0) {
    if (S_ISDIR(st.st_mode)) return 0;
    return -1;
  }
  if (mkdir(dir, 0777) == 0) return 0;
  return -1;
}

// create fifo if it does not exist
static int ensure_fifo_exists(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISFIFO(st.st_mode)) return 0;
    return -1;
  }
  if (mkfifo(path, 0666) == 0) return 0;
  if (errno == EEXIST) return 0;
  return -1;
}

// This child keeps reading from our own fifo and prints messages to screen.
static void chatroom_reader_loop(const char *fifo_path) {
  int fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) exit(1);

  char buf[1024];
  while (1) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      write(STDOUT_FILENO, buf, (size_t)n);
    } else {
      // small sleep to avoid busy loop
      usleep(20000);
    }
  }
}

// Builtin command: chatroom <roomname> <username>
// Uses /tmp/chatroom-<roomname>/ and named pipes for each user.
static int builtin_chatroom(struct command_t *command) {
  if (command->args[1] == NULL || command->args[2] == NULL) {
    fprintf(stderr, "-%s: chatroom: usage: chatroom <roomname> <username>\n", sysname);
    return SUCCESS;
  }

  const char *room = command->args[1];
  const char *user = command->args[2];

  char roomdir[PATH_MAX];
  snprintf(roomdir, sizeof(roomdir), "/tmp/chatroom-%s", room);

  // create room directory if needed
  if (ensure_dir_exists(roomdir) != 0) {
    fprintf(stderr, "-%s: chatroom: %s\n", sysname, strerror(errno));
    return SUCCESS;
  }

  char myfifo[PATH_MAX];
  snprintf(myfifo, sizeof(myfifo), "%s/%s", roomdir, user);

  // create our fifo if needed
  if (ensure_fifo_exists(myfifo) != 0) {
    fprintf(stderr, "-%s: chatroom: %s\n", sysname, strerror(errno));
    return SUCCESS;
  }

  printf("Welcome to %s!\n", room);

  // fork reader process that prints incoming messages
  pid_t reader = fork();
  if (reader == 0) {
    chatroom_reader_loop(myfifo);
    exit(0);
  }

  // main loop: read user input and broadcast to others
  char line[512];
  while (1) {
    printf("[%s] %s > ", room, user);
    fflush(stdout);

    if (fgets(line, sizeof(line), stdin) == NULL) break;
    if (strcmp(line, "/exit\n") == 0 || strcmp(line, "/exit") == 0) break;

    // format message like [room] user: msg
    char msg[1024];
    int m = snprintf(msg, sizeof(msg), "[%s] %s: %s", room, user, line);
    if (m <= 0) continue;

    DIR *d = opendir(roomdir);
    if (!d) continue;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
      // skip . and ..
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
      // don't write to our own fifo
      if (strcmp(ent->d_name, user) == 0) continue;

      char otherfifo[PATH_MAX];
      snprintf(otherfifo, sizeof(otherfifo), "%s/%s", roomdir, ent->d_name);

      // send message by creating a child (so writing does not block us)
      pid_t sp = fork();
      if (sp == 0) {
        int fd = open(otherfifo, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
          write(fd, msg, (size_t)m);
          close(fd);
        }
        exit(0);
      }
    }
    closedir(d);

    // clean up finished sender children
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
  }

  // stop reader process before exiting
  if (reader > 0) {
    kill(reader, SIGKILL);
    waitpid(reader, NULL, 0);
  }

  // remove our fifo when leaving
  unlink(myfifo);
  return SUCCESS;
}

// Builtin command: cut (like Unix cut)
// Reads stdin line by line and prints selected fields.
static int builtin_cut(struct command_t *command) {
  char delim = '\t';           // default delimiter is TAB
  char *fields_spec = NULL;    // example: "1,3,10"

  // parse flags -d/--delimiter and -f/--fields
  for (int i = 1; command->args[i] != NULL; i++) {
    char *a = command->args[i];

    if (strcmp(a, "-d") == 0) {
      if (command->args[i + 1] != NULL && command->args[i + 1][0] != '\0') {
        delim = command->args[i + 1][0];
        i++;
      }
      continue;
    }
    if (strncmp(a, "-d", 2) == 0) {
      if (a[2] != '\0') delim = a[2];
      continue;
    }

    if (strcmp(a, "--delimiter") == 0) {
      if (command->args[i + 1] != NULL && command->args[i + 1][0] != '\0') {
        delim = command->args[i + 1][0];
        i++;
      }
      continue;
    }
    if (strncmp(a, "--delimiter=", 12) == 0) {
      if (a[12] != '\0') delim = a[12];
      continue;
    }

    if (strcmp(a, "-f") == 0) {
      if (command->args[i + 1] != NULL) {
        fields_spec = command->args[i + 1];
        i++;
      }
      continue;
    }
    if (strncmp(a, "-f", 2) == 0) {
      if (a[2] != '\0') fields_spec = a + 2;
      continue;
    }

    if (strcmp(a, "--fields") == 0) {
      if (command->args[i + 1] != NULL) {
        fields_spec = command->args[i + 1];
        i++;
      }
      continue;
    }
    if (strncmp(a, "--fields=", 9) == 0) {
      if (a[9] != '\0') fields_spec = a + 9;
      continue;
    }
  }

  // if fields not given, do nothing
  if (fields_spec == NULL || fields_spec[0] == '\0') {
    return SUCCESS;
  }

  int fields[256];
  int fields_n = 0;

  // copy fields string because strtok will modify it
  char *spec_copy = strdup(fields_spec);
  if (spec_copy == NULL) return SUCCESS;

  // split "1,3,10" by comma
  char *saveptr = NULL;
  char *tok = strtok_r(spec_copy, ",", &saveptr);
  while (tok != NULL && fields_n < 256) {
    int v = parse_positive_int(tok);
    if (v > 0) fields[fields_n++] = v;
    tok = strtok_r(NULL, ",", &saveptr);
  }

  free(spec_copy);

  if (fields_n == 0) {
    return SUCCESS;
  }

  // read input line by line
  char *line = NULL;
  size_t cap = 0;

  while (1) {
    ssize_t nread = getline(&line, &cap, stdin);
    if (nread < 0) break;

    // remove newline if exists
    int has_nl = 0;
    if (nread > 0 && line[nread - 1] == '\n') {
      has_nl = 1;
      line[nread - 1] = '\0';
      nread--;
    }

    // we will split line by delimiter and store pointers to each field
    char *starts[1024];
    char *ends[1024];
    int count = 0;

    char *p = line;
    starts[count] = p;

    while (*p != '\0') {
      if (*p == delim) {
        ends[count] = p;
        *p = '\0';         // terminate this field
        count++;
        if (count >= 1023) break;
        starts[count] = p + 1; // start of next field
      }
      p++;
    }

    ends[count] = p;
    count++;

    // print requested fields in the given order
    int first_out = 1;
    for (int i = 0; i < fields_n; i++) {
      int idx = fields[i] - 1;
      if (idx >= 0 && idx < count) {
        if (!first_out) putchar(delim);
        fputs(starts[idx], stdout);
        first_out = 0;
      }
    }

    if (has_nl) putchar('\n');
  }

  if (line) free(line);
  return SUCCESS;
}

// Run a pipe chain like: cmd1 | cmd2 | cmd3
// We fork each command and connect them with pipe() and dup2().
static int run_pipeline(struct command_t *command) {
  int prev_read = -1;     // read end of previous pipe
  pid_t pids[256];
  int pid_count = 0;

  struct command_t *cur = command;

  while (cur != NULL) {
    int pipefd[2] = {-1, -1};

    // create pipe only if there is a next command
    if (cur->next != NULL) {
      if (pipe(pipefd) < 0) {
        fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
        return SUCCESS;
      }
    }

    pid_t pid = fork();
    if (pid == 0) {
      // child: connect stdin from prev pipe if exists
      if (prev_read != -1) {
        dup2(prev_read, STDIN_FILENO);
      }
      // child: connect stdout to next pipe if exists
      if (cur->next != NULL) {
        dup2(pipefd[1], STDOUT_FILENO);
      }

      // close unused fds in child
      if (prev_read != -1) close(prev_read);
      if (pipefd[0] != -1) close(pipefd[0]);
      if (pipefd[1] != -1) close(pipefd[1]);

      // apply <, >, >> for this command
      apply_redirects(cur);

      // support builtin commands in pipes
      if (strcmp(cur->name, "cut") == 0) {
        builtin_cut(cur);
        exit(0);
      }
      if (strcmp(cur->name, "pinfo") == 0) {
        builtin_pinfo(cur);
        exit(0);
      }
      if (strcmp(cur->name, "chatroom") == 0) {
        // chatroom is interactive, so we don't allow it in a pipe
        fprintf(stderr, "-%s: chatroom cannot be used in a pipe\n", sysname);
        exit(1);
      }

      // external command: find full path and execv
      char *full_path = resolve_path(cur->name);
      if (full_path != NULL) {
        execv(full_path, cur->args);
        // execv returns only if there is an error
        fprintf(stderr, "-%s: %s: %s\n", sysname, cur->name, strerror(errno));
        free(full_path);
        exit(126);
      } else {
        fprintf(stderr, "-%s: %s: command not found\n", sysname, cur->name);
        exit(127);
      }
    }

    // parent error case
    if (pid < 0) {
      fprintf(stderr, "-%s: %s\n", sysname, strerror(errno));
      if (prev_read != -1) close(prev_read);
      if (pipefd[0] != -1) close(pipefd[0]);
      if (pipefd[1] != -1) close(pipefd[1]);
      return SUCCESS;
    }

    // save pid to wait later
    if (pid_count < 256) {
      pids[pid_count++] = pid;
    }

    // parent closes ends that it does not need
    if (prev_read != -1) close(prev_read);
    if (pipefd[1] != -1) close(pipefd[1]);

    // next command reads from current pipe read end
    prev_read = pipefd[0];

    cur = cur->next;
  }

  if (prev_read != -1) close(prev_read);

  // if background, do not wait for all children (just cleanup finished ones)
  if (command->background) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    return SUCCESS;
  } else {
    // foreground: wait all commands in pipe chain
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

  // builtin: cd changes current directory of the shell process
  if (strcmp(command->name, "cd") == 0) {
    if (command->arg_count > 0) {
      r = chdir(command->args[1]);
      if (r == -1)
        printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
      return SUCCESS;
    }
  }

  // if we have pipe chain, run_pipeline will fork multiple children
  if (command->next != NULL) {
    return run_pipeline(command);
  }

  pid_t pid = fork();
  if (pid == 0) // child
  {
    // in child: apply redirection before running command
    apply_redirects(command);

    // builtin commands (Part III)
    if (strcmp(command->name, "cut") == 0) {
      builtin_cut(command);
      exit(0);
    }

    if (strcmp(command->name, "pinfo") == 0) {
      builtin_pinfo(command);
      exit(0);
    }

    if (strcmp(command->name, "chatroom") == 0) {
      builtin_chatroom(command);
      exit(0);
    }

    // external commands: resolve PATH and run with execv (Part I)
    char *full_path = resolve_path(command->name);

    if (full_path != NULL) {
      execv(full_path, command->args);
      fprintf(stderr, "-%s: %s: %s\n", sysname, command->name, strerror(errno));
      free(full_path);
      exit(126);
    } else {
      // resolve_path returned NULL (command not found in PATH)
      fprintf(stderr, "-%s: %s: command not found\n", sysname, command->name);
      exit(127); // 127 is standard for "Command not found"
    }
  } else {
    // parent: background means do not wait
    if (command->background) {
      // we can reap finished background children with WNOHANG
      while (waitpid(-1, NULL, WNOHANG) > 0) {}
      return SUCCESS;
    } else {
      // foreground: wait until command finishes
      waitpid(pid, NULL, 0);
      return SUCCESS;
    }
  }
}

int main() {
  while (1) {
    // allocate and clear new command struct for each input line
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

    // free everything we allocated for this command
    free_command(command);
  }

  printf("\n");
  return 0;
}
