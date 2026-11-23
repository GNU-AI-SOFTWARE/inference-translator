/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
 *
 * ptyrun.c -- minimal pseudo-terminal driver for test_client.sh.
 *
 * util-linux does not build script(1) on GNU/Hurd (Debian #807071:
 * its signalfd(2) use is Linux-specific), so the end-to-end client
 * test cannot rely on `script -qec ...` everywhere.  ptyrun runs a
 * command on a fresh pty, forwarding our own stdin to the pty master
 * and the master output to our stdout, like:
 *
 *     printf 'line1\n/quit\n' | ptyrun ./src/inference --mount DIR
 *
 * The child gets the pty as its controlling terminal (setsid + open
 * of the slave without O_NOCTTY), so isatty(0) && isatty(1) hold and
 * the interactive client starts.  Exit status: the child's exit
 * status, or 124 when the 60 s watchdog fires.
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int dbg;
#define DBG(...) do { if (dbg) fprintf (stderr, "ptyrun: " __VA_ARGS__); } while (0)

static pid_t child_pid;

static void
on_alarm (int sig)
{
  (void) sig;
  if (child_pid > 0)
    kill (child_pid, SIGKILL);
  _exit (124);
}

int
main (int argc, char **argv)
{
  if (argc < 2)
    {
      fprintf (stderr, "usage: ptyrun CMD [ARG...]\n");
      return 2;
    }
  dbg = getenv ("PTYRUN_DEBUG") != NULL;

  int master = posix_openpt (O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt (master) != 0 || unlockpt (master) != 0)
    {
      perror ("posix_openpt");
      return 2;
    }
  char *slave_name = ptsname (master);
  if (slave_name == NULL)
    {
      perror ("ptsname");
      return 2;
    }
  DBG ("master=%d slave=%s\n", master, slave_name);

  signal (SIGALRM, on_alarm);
  alarm (60);

  child_pid = fork ();
  if (child_pid < 0)
    {
      perror ("fork");
      return 2;
    }
  if (child_pid > 0)
    DBG ("child pid=%d\n", (int) child_pid);
  if (child_pid == 0)
    {
      setsid ();
      int slave = open (slave_name, O_RDWR);
      if (slave < 0)
        _exit (127);
      dup2 (slave, STDIN_FILENO);
      dup2 (slave, STDOUT_FILENO);
      dup2 (slave, STDERR_FILENO);
      if (slave > STDERR_FILENO)
        close (slave);
      close (master);
      execvp (argv[1], argv + 1);
      _exit (127);
    }

  int stdin_open = 1;
  for (;;)
    {
      struct pollfd pfd[2];
      nfds_t n = 0;
      size_t m;
      if (stdin_open)
        {
          pfd[n].fd = STDIN_FILENO;
          pfd[n].events = POLLIN;
          pfd[n].revents = 0;
          n++;
        }
      pfd[n].fd = master;
      pfd[n].events = POLLIN;
      pfd[n].revents = 0;
      n++;
      m = n - 1;

      if (poll (pfd, n, -1) < 0)
        {
          if (errno == EINTR)
            continue;
          break;
        }

      if (stdin_open && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
        {
          char buf[4096];
          ssize_t r = read (STDIN_FILENO, buf, sizeof buf);
          DBG ("stdin read %zd\n", r);
          if (r <= 0)
            stdin_open = 0;     /* keep the master open: no EOF for the child */
          else
            {
              DBG ("stdin->master %zd bytes\n", r);
              ssize_t off = 0;
              while (off < r)
                {
                  ssize_t w = write (master, buf + off, (size_t) (r - off));
                  if (w <= 0)
                    break;
                  off += w;
                }
            }
        }

      if (pfd[m].revents & (POLLIN | POLLHUP | POLLERR))
        {
          char buf[4096];
          ssize_t r = read (master, buf, sizeof buf);
          DBG ("master read %zd errno=%d\n", r, r <= 0 ? errno : 0);
          if (r <= 0)
            break;              /* child gone: the pty master reports EOF/EIO */
          ssize_t off = 0;
          while (off < r)
            {
              ssize_t w = write (STDOUT_FILENO, buf + off, (size_t) (r - off));
              if (w <= 0)
                break;
              off += w;
            }
        }
    }

  close (master);
  int status = 0;
  while (waitpid (child_pid, &status, 0) < 0 && errno == EINTR)
    ;
  DBG ("child exit=%d\n", WIFEXITED (status) ? WEXITSTATUS (status) : -1);
  if (WIFEXITED (status))
    return WEXITSTATUS (status);
  return 1;
}
