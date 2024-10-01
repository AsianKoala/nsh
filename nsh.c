#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_LINE 1024
#define MAX_ARGS 128
#define MAX_CMDS 64

typedef struct job {
    pid_t pid;
    char command[MAX_LINE];
    struct job *next;
} job;

job *job_list = NULL;

void add_job(pid_t pid, char *command) {
    job *new_job = malloc(sizeof(job));
    new_job->pid = pid;
    strcpy(new_job->command, command);
    new_job->next = job_list;
    job_list = new_job;
}

void remove_job(pid_t pid) {
    job **current = &job_list;
    while (*current) {
        if ((*current)->pid == pid) {
            job *tmp = *current;
            *current = (*current)->next;
            free(tmp);
            return;
        }
        current = &(*current)->next;
    }
}

void print_jobs() {
    job *current = job_list;
    while (current) {
        printf("[%d] %s\n", current->pid, current->command);
        current = current->next;
    }
}

void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        remove_job(pid);
    }
}

void execute_command(char *cmdline);

int main() {
    char cmdline[MAX_LINE];

    signal(SIGCHLD, sigchld_handler);

    while (1) {
        printf("mini_shell> ");
        fflush(stdout);

        if (!fgets(cmdline, MAX_LINE, stdin)) {
            break; // EOF
        }

        if (strcmp(cmdline, "\n") == 0) {
            continue; // Empty command
        }

        execute_command(cmdline);
    }

    return 0;
}

void parse_command(char *cmdline, char **commands, int *background) {
    char *token;
    int cmd_count = 0;

    if (cmdline[strlen(cmdline) - 1] == '\n') {
        cmdline[strlen(cmdline) - 1] = '\0'; // Remove trailing newline
    }

    // Check for background execution
    if (cmdline[strlen(cmdline) - 1] == '&') {
        *background = 1;
        cmdline[strlen(cmdline) - 1] = '\0';
    }

    token = strtok(cmdline, "|");
    while (token != NULL && cmd_count < MAX_CMDS - 1) {
        commands[cmd_count++] = token;
        token = strtok(NULL, "|");
    }
    commands[cmd_count] = NULL;
}

void parse_args(char *command, char **argv) {
    char *token;
    int argc = 0;

    token = strtok(command, " ");
    while (token != NULL && argc < MAX_ARGS - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;
}

void execute_command(char *cmdline) {
    char *commands[MAX_CMDS];
    int background = 0;
    char *cmd_copy = strdup(cmdline);

    // Built-in 'jobs' command
    if (strncmp(cmdline, "jobs", 4) == 0) {
        print_jobs();
        free(cmd_copy);
        return;
    }

    // Parse command line into individual commands separated by '|'
    parse_command(cmdline, commands, &background);

    int num_cmds = 0;
    while (commands[num_cmds] != NULL) {
        num_cmds++;
    }

    // Handle empty input
    if (num_cmds == 0) {
        free(cmd_copy);
        return;
    }

    int pipefds[2 * (num_cmds - 1)];
    for (int i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipefds + i * 2) < 0) {
            perror("pipe");
            free(cmd_copy);
            return;
        }
    }

    pid_t pid;
    int status;
    int cmd_index = 0;

    for (int i = 0; i < num_cmds; i++) {
        char *argv[MAX_ARGS];
        parse_args(commands[i], argv);

        pid = fork();
        if (pid == 0) { // Child process
            // Input redirection if first command
            if (i == 0) {
                for (int j = 0; argv[j] != NULL; j++) {
                    if (strcmp(argv[j], "<") == 0) {
                        int fd = open(argv[j + 1], O_RDONLY);
                        if (fd < 0) {
                            perror("open");
                            exit(EXIT_FAILURE);
                        }
                        dup2(fd, STDIN_FILENO);
                        close(fd);
                        argv[j] = NULL;
                        break;
                    }
                }
            }

            // Output redirection if last command
            if (i == num_cmds - 1) {
                for (int j = 0; argv[j] != NULL; j++) {
                    if (strcmp(argv[j], ">") == 0) {
                        int fd = open(argv[j + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (fd < 0) {
                            perror("open");
                            exit(EXIT_FAILURE);
                        }
                        dup2(fd, STDOUT_FILENO);
                        close(fd);
                        argv[j] = NULL;
                        break;
                    } else if (strcmp(argv[j], ">>") == 0) {
                        int fd = open(argv[j + 1], O_WRONLY | O_CREAT | O_APPEND, 0644);
                        if (fd < 0) {
                            perror("open");
                            exit(EXIT_FAILURE);
                        }
                        dup2(fd, STDOUT_FILENO);
                        close(fd);
                        argv[j] = NULL;
                        break;
                    }
                }
            }

            // If not the first command, redirect stdin to the previous pipe's read end
            if (i > 0) {
                dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
            }

            // If not the last command, redirect stdout to the current pipe's write end
            if (i < num_cmds - 1) {
                dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
            }

            // Close all pipe file descriptors in child
            for (int j = 0; j < 2 * (num_cmds - 1); j++) {
                close(pipefds[j]);
            }

            // Execute the command
            execvp(argv[0], argv);
            perror("execvp");
            exit(EXIT_FAILURE);
        } else if (pid < 0) {
            perror("fork");
            free(cmd_copy);
            return;
        }

        cmd_index++;
    }

    // Close all pipe file descriptors in parent
    for (int i = 0; i < 2 * (num_cmds - 1); i++) {
        close(pipefds[i]);
    }

    if (background) {
        add_job(pid, cmd_copy);
        printf("[%d] %s\n", pid, cmd_copy);
    } else {
        for (int i = 0; i < num_cmds; i++) {
            wait(&status);
        }
    }

    free(cmd_copy);
}
