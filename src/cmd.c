// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "cmd.h"
#include "utils.h"

#define READ		0
#define WRITE		1
#define MAX_PATH  256


/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	if (!dir || dir->next_word)
		return true;

	if (chdir(dir->string))
		return false;

	return true;
}

/**
 * Internal show current path command.
 */
static int shell_pwd(simple_command_t *s)
{
	int fd_file, fd_copy_stdout, ret;

	if (s->out) {
		char out_file[MAX_PATH];

		out_file[0] = '\0';
		word_t *part = s->out;

		while (part) {
			if (part->expand == true) {
				char *substring = getenv(part->string);

				if (!substring)
					substring = "";

				strcat(out_file, substring);

			} else {
				strcat(out_file, part->string);
			}

			part = part->next_part;
		}

		/**
		 * Sets the outfile the stdout.
		 */
		fd_file = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		DIE(fd_file < 0, "open failed\n");

		fd_copy_stdout = dup(STDOUT_FILENO);
		DIE(fd_copy_stdout < 0, "dup failed\n");

		ret = dup2(fd_file, STDOUT_FILENO);
		DIE(ret < 0, "dup2 failed\n");

		ret = close(fd_file);
		DIE(ret < 0, "close failed\n");
	}

	char *path = getcwd(NULL, MAX_PATH);

	DIE(!path, "getcwd failed\n");

	strcat(path, "\n");

	write(STDOUT_FILENO, path, strlen(path));

	free(path);

	/**
	 * stdout reset.
	 */
	if (s->out) {
		ret = dup2(fd_copy_stdout, STDOUT_FILENO);
		DIE(ret < 0, "dup2 failed\n");

		ret = close(fd_copy_stdout);
		DIE(ret < 0, "close failed");
	}

	return EXIT_SUCCESS;
}

/**
 * Redirect a child process.
 */
static void redirect_child_process(simple_command_t *s)
{
	int fd_file, ret, open_modes = O_WRONLY | O_CREAT | O_TRUNC;

	if (s->in) { // "<"
		fd_file = open(s->in->string, O_RDONLY);
		DIE(fd_file < 0, "open failed\n");

		ret = dup2(fd_file, STDIN_FILENO);
		DIE(ret < 0, "dup2 failed\n");
	}

	if (s->out) { // ">"   ">>"
		if (s->io_flags == IO_OUT_APPEND)
			open_modes = O_WRONLY | O_CREAT | O_APPEND;

		fd_file = open(s->out->string, open_modes, 0644);
		DIE(fd_file < 0, "open failed\n");

		ret = dup2(fd_file, STDOUT_FILENO);
		DIE(ret < 0, "dup2 failed\n");
	}

	if (s->err) { // "2>"   "2>>"
		if (s->io_flags == IO_ERR_APPEND)
			open_modes = O_WRONLY | O_CREAT | O_APPEND;

		if (!s->out || (s->out && strcmp(s->out->string, s->err->string))) { // "&>"
			fd_file = open(s->err->string, open_modes, 0644);
			DIE(fd_file < 0, "open failed\n");
		}

		ret = dup2(fd_file, STDERR_FILENO);
		DIE(ret < 0, "dup2 failed\n");
	}

	ret = close(fd_file);
	DIE(ret < 0, "close failed\n");
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	DIE(!s, "simple command failed\n");

	if (!strcmp(s->verb->string, "exit") ||
		!strcmp(s->verb->string, "quit")) {
		return SHELL_EXIT;
	}

	if (!strcmp(s->verb->string, "cd")) {
		/**
		 * For cd redirections, creating files case is treated.
		 */
		if (s->out) {
			int fd_file = open(s->out->string, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			DIE(fd_file < 0, "open failed\n");

			int ret = close(fd_file);

			DIE(ret < 0, "close failed\n");
		}

		if (!shell_cd(s->params))
			return EXIT_FAILURE;

		return EXIT_SUCCESS;
	}

	if (!strcmp(s->verb->string, "pwd"))
		return shell_pwd(s);

	if (s->verb->next_part && !strcmp(s->verb->next_part->string, "=")) {
		char variable_value[MAX_PATH];

		variable_value[0] = '\0';

		word_t *value_parts  = s->verb->next_part->next_part;

		/**
		 * Creating the variable value.
		 */
		while (value_parts) {
			if (value_parts->expand == true) {
				char *substring = getenv(value_parts->string);

				if (!substring)
					substring = "";

				strcat(variable_value, substring);

			} else {
				strcat(variable_value, value_parts->string);
			}

			value_parts = value_parts->next_part;
		}

		int ret = setenv(s->verb->string, variable_value, 1);

		DIE(ret < 0, "setenv failed\n");

		return EXIT_SUCCESS;
	}

	int status;

	pid_t pid = fork();

	DIE(pid == -1, "fork failed\n");

	if (pid == 0) {
		int param_size;

		if (s->in || s->out || s->err)
			redirect_child_process(s);

		execvp(s->verb->string, get_argv(s, &param_size));

		/**
		 * If execvp succeeds the exits, else
		 * an output string is created.
		 */
		char string[MAX_PATH];

		string[0] = '\0';

		strcat(string, "Execution failed for '");
		strcat(string, s->verb->string);
		strcat(string, "'\n");

		write(STDERR_FILENO, string, strlen(string));
		exit(EXIT_FAILURE);

	} else {
		waitpid(pid, &status, 0);
	}

	return WEXITSTATUS(status);
}

/**
 * Chain multiple commands that will run sequentially, one after another.
 */
static int run_in_sequential(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	parse_command(cmd1, level, father);

	return parse_command(cmd2, level, father);
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	int status1, status2;

	pid_t pid1 = fork();

	DIE(pid1 < 0, "fork failed\n");

	if (pid1 == 0)
		exit(parse_command(cmd1, level, father));

	pid_t pid2 = fork();

	DIE(pid2 < 0, "fork failed\n");

	if (pid2 == 0)
		exit(parse_command(cmd2, level, father));

	waitpid(pid1, &status1, 0);
	waitpid(pid2, &status2, 0);

	return WEXITSTATUS(status1) && WEXITSTATUS(status2);
}

/**
 * The second command runs only if the first one fails.
 */
static int run_if_first_fails(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	if (parse_command(cmd1, level, father) == EXIT_SUCCESS)
		return EXIT_SUCCESS;

	return parse_command(cmd2, level, father);
}

/**
 * The second command runs only if the first one succeeds.
 */
static int run_if_first_succeeds(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	if (parse_command(cmd1, level, father) == EXIT_FAILURE)
		return EXIT_FAILURE;

	return parse_command(cmd2, level, father);
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	int pipe_fd[2], status;

	int ret = pipe(pipe_fd);

	DIE(ret < 0, "pipe failed\n");

	pid_t pid1 = fork();

	DIE(pid1 < 0, "fork failed\n");

	if (pid1 == 0) {
		int fd_copy_stdout = dup(STDOUT_FILENO);

		DIE(fd_copy_stdout < 0, "dup failed\n");

		/**
		 * sets pipe_fd[1] the stdout.
		 */
		ret = dup2(pipe_fd[1], STDOUT_FILENO);
		DIE(ret < 0, "dup2 failed\n");

		ret = close(pipe_fd[0]);
		DIE(ret < 0, "close failed\n");

		ret = close(pipe_fd[1]);
		DIE(ret < 0, "close failed\n");

		int ret1 = parse_command(cmd1, level, father);

		/**
		 * stdout reset.
		 */
		ret = dup2(fd_copy_stdout, STDOUT_FILENO);
		DIE(ret < 0, "dup2 failed\n");

		ret = close(fd_copy_stdout);
		DIE(ret < 0, "close failed\n");

		exit(ret1);

	} else {
		waitpid(pid1, NULL, 0);

		pid_t pid2 = fork();

		DIE(pid2 < 0, "fork failed\n");

		if (pid2 == 0) {
			int fd_copy_stdin = dup(STDIN_FILENO);

			DIE(fd_copy_stdin < 0, "dup failed\n");

			/**
			 * sets pipe_fd[0] the stdin.
			 */
			ret = dup2(pipe_fd[0], STDIN_FILENO);
			DIE(ret < 0, "dup2 failed\n");

			ret = close(pipe_fd[0]);
			DIE(ret < 0, "close failed\n");

			ret = close(pipe_fd[1]);
			DIE(ret < 0, "close failed\n");

			int ret2 = parse_command(cmd2, level, father);

			/**
			 * stdin reset.
			 */
			ret = dup2(fd_copy_stdin, STDIN_FILENO);
			DIE(ret < 0, "dup2 failed\n");

			ret = close(fd_copy_stdin);
			DIE(ret < 0, "close failed\n");

			exit(ret2);

		} else {
			/**
			 * the main process closes its fds.
			 */
			ret = close(pipe_fd[0]);
			DIE(ret < 0, "close failed\n");

			ret = close(pipe_fd[1]);
			DIE(ret < 0, "close failed\n");

			waitpid(pid2, &status, 0);
		}
	}

	return WEXITSTATUS(status);
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	switch (c->op) {
	case OP_NONE:
		return parse_simple(c->scmd, level, c);

	case OP_SEQUENTIAL:  // ;
		return run_in_sequential(c->cmd1, c->cmd2, level, c);

	case OP_PARALLEL: // &
		return run_in_parallel(c->cmd1, c->cmd2, level, c);

	case OP_CONDITIONAL_NZERO: // ||
		return run_if_first_fails(c->cmd1, c->cmd2, level, c);

	case OP_CONDITIONAL_ZERO: // &&
		return run_if_first_succeeds(c->cmd1, c->cmd2, level, c);

	case OP_PIPE: // |
		return run_on_pipe(c->cmd1, c->cmd2, level, c);

	default:
		return SHELL_EXIT;
	}
}
