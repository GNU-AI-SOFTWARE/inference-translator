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
 *
 * stdin is held in a buffer and forwarded to the master only once the
 * child's output burst has been quiet for 300 ms (or after 5 s with no
 * output at all, so a silent child still gets its stdin): the
 * interactive client finishes its termios setup within that first
 * burst, so piped input then lands in the raw queue and can never be
 * flushed by an ICANON clear on pty servers that discard the canonical
 * queue on mode change (GNU/Hurd).  poll() is always called with
 * timeout -1 -- on GNU/Hurd a finite timeout on a lone pty master
 * never wakes -- and the wait for the quiet period is paced with
 * nanosleep() while stdin sits at EOF (POLLHUP spins).
 */

#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double
mono (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static int
write_all (int fd, const char *buf, size_t len)
{
  while (len > 0)
    {
      ssize_t w = write (fd, buf, len);
      if (w <= 0)
        return -1;
      buf += w;
      len -= (size_t) w;
    }
  return 0;
}

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

  signal (SIGALRM, on_alarm);
  alarm (60);

  child_pid = fork ();
  if (child_pid < 0)
    {
      perror ("fork");
      return 2;
    }
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

  int stdin_open = 1;     /* keep polling stdin until ready && EOF */
  int stdin_eof = 0;
  int ready = 0;          /* forward stdin only after the child's first output
                             burst went quiet (its termios setup is inside
                             that burst), or after 5 s if it stays silent */
  int seen_output = 0;
  double t_start = mono (), t_last_out = 0.0;
  char hold[65536];       /* stdin is drained here until ready */
  size_t hold_len = 0;

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

      /* Always poll(-1): on GNU/Hurd a finite timeout on a lone pty master
         never wakes, so no timeout is used and the quiet-period wait is
         paced with nanosleep() below. */
      if (poll (pfd, n, -1) < 0)
        {
          if (errno == EINTR)
            continue;
          break;
        }

      int progress = 0;
      if (stdin_open && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
        {
          char buf[4096];
          ssize_t r = read (STDIN_FILENO, buf, sizeof buf);
          if (r > 0)
            {
              if (ready)
                {
                  if (write_all (master, buf, (size_t) r) < 0)
                    break;
                }
              else if (hold_len + (size_t) r <= sizeof hold)
                {
                  memcpy (hold + hold_len, buf, (size_t) r);
                  hold_len += (size_t) r;
                }
              progress = 1;
            }
          else
            {
              stdin_eof = 1;    /* keep the master open: no EOF for the child */
              if (ready)
                stdin_open = 0;
            }
        }

      if (pfd[m].revents & (POLLIN | POLLHUP | POLLERR))
        {
          char buf[4096];
          ssize_t r = read (master, buf, sizeof buf);
          if (r <= 0)
            break;              /* child gone: the pty master reports EOF/EIO */
          if (write_all (STDOUT_FILENO, buf, (size_t) r) < 0)
            break;
          seen_output = 1;
          t_last_out = mono ();
          progress = 1;
        }

      if (!ready)
        {
          double now = mono ();
          if ((seen_output && now - t_last_out >= 0.300)
              || (!seen_output && now - t_start >= 5.0))
            {
              ready = 1;
              if (hold_len > 0)
                {
                  if (write_all (master, hold, hold_len) < 0)
                    break;
                  hold_len = 0;
                }
              if (stdin_eof)
                stdin_open = 0;
            }
          else if (!progress)
            {
              struct timespec ts = { 0, 20 * 1000 * 1000 };
              nanosleep (&ts, NULL);  /* pace the spin while waiting */
            }
        }
    }

  close (master);
  int status = 0;
  while (waitpid (child_pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (WIFEXITED (status))
    return WEXITSTATUS (status);
  return 1;
}
