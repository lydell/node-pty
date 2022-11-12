/**
 * Copyright (c) 2012-2015, Christopher Jeffrey (MIT License)
 * Copyright (c) 2017, Daniel Imms (MIT License)
 *
 * pty.cc:
 *   This file is responsible for starting processes
 *   with pseudo-terminal file descriptors.
 *
 * See:
 *   man pty
 *   man tty_ioctl
 *   man termios
 *   man forkpty
 */

/**
 * Includes
 */

#include <napi.h>
#include <uv.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>

#include "comms.h"

/* forkpty */
/* http://www.gnu.org/software/gnulib/manual/html_node/forkpty.html */
#if defined(__GLIBC__) || defined(__CYGWIN__)
#include <pty.h>
#elif defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#elif defined(__sun)
#include <stropts.h> /* for I_PUSH */
#else
#include <pty.h>
#endif

#include <termios.h> /* tcgetattr, tty_ioctl */

/* Some platforms name VWERASE and VDISCARD differently */
#if !defined(VWERASE) && defined(VWERSE)
#define VWERASE	VWERSE
#endif
#if !defined(VDISCARD) && defined(VDISCRD)
#define VDISCARD	VDISCRD
#endif

/* for pty_getproc */
#if defined(__linux__)
#include <stdio.h>
#include <stdint.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <libproc.h>
#endif

/* NSIG - macro for highest signal + 1, should be defined */
#ifndef NSIG
#define NSIG 32
#endif

#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  #define HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT 1
#else
  #define HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT 0
  #define POSIX_SPAWN_CLOEXEC_DEFAULT 0
#endif

#ifndef POSIX_SPAWN_USEVFORK
  #define POSIX_SPAWN_USEVFORK 0
#endif

/**
 * Structs
 */

struct pty_baton {
  Napi::Env *env;
  Napi::FunctionReference cb;
  int exit_code;
  int signal_code;
  pid_t pid;
  uv_async_t async;
  uv_thread_t tid;
};

/**
 * Methods
 */

Napi::Value PtyFork(const Napi::CallbackInfo& info);
Napi::Value PtyOpen(const Napi::CallbackInfo& info);
Napi::Value PtyResize(const Napi::CallbackInfo& info);
Napi::Value PtyGetProc(const Napi::CallbackInfo& info);

/**
 * Functions
 */

static int
pty_nonblock(int);

static char *
pty_getproc(int, char *);

static int
pty_openpty(int *, int *, char *,
            const struct termios *,
            const struct winsize *);

static void
pty_waitpid(void *);

static void
pty_after_waitpid(uv_async_t *);

static void
pty_after_close(uv_handle_t *);

static void throw_for_errno(Napi::Env env, const char* message, int _errno) {
  Napi::Error::New(env, (
    message + std::string(strerror(_errno))
  ).c_str()).ThrowAsJavaScriptException();
}

Napi::Value PtyFork(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() != 12 ||
      !info[0].IsString() ||
      !info[1].IsArray() ||
      !info[2].IsArray() ||
      !info[3].IsString() ||
      !info[4].IsNumber() ||
      !info[5].IsNumber() ||
      !info[6].IsNumber() ||
      !info[7].IsNumber() ||
      !info[8].IsBoolean() ||
      !info[9].IsBoolean() ||
      !info[10].IsFunction() ||
      !info[11].IsString()) {
    Napi::Error::New(env,
        "Usage: pty.fork(file, args, env, cwd, cols, rows, uid, gid, closeFDs, utf8, onexit, helperPath)").ThrowAsJavaScriptException();
    return env.Null();
  }

  // file
  std::string file = info[0].As<Napi::String>().Utf8Value();

  // envp
  Napi::Array env_ = info[2].As<Napi::Array>();
  int envc = env_.Length();
  char **envp = new char*[envc+1];
  envp[envc] = NULL;
  for (int i = 0; i < envc; i++) {
    std::string pair = env_.Get(i).As<Napi::String>().Utf8Value();
    envp[i] = strdup(pair.c_str());
  }

  // cwd
  std::string cwd_ = info[3].As<Napi::String>().Utf8Value();

  // uid / gid
  int uid = info[6].As<Napi::Number>();
  int gid = info[7].As<Napi::Number>();

  // closeFDs
  bool closeFDs = info[8].As<Napi::Boolean>().Value();
  bool explicitlyCloseFDs = closeFDs && !HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT;

  // args
  Napi::Array argv_ = info[1].As<Napi::Array>();

  const int EXTRA_ARGS = 5;
  int argc = argv_.Length();
  int argl = argc + EXTRA_ARGS + 1;
  char **argv = new char*[argl];
  argv[0] = strdup(cwd_.c_str());
  argv[1] = strdup(std::to_string(uid).c_str());
  argv[2] = strdup(std::to_string(gid).c_str());
  argv[3] = strdup(explicitlyCloseFDs ? "1": "0");
  argv[4] = strdup(file.c_str());
  argv[argl - 1] = NULL;
  for (int i = 0; i < argc; i++) {
    std::string arg = argv_.Get(i).As<Napi::String>().Utf8Value();
    argv[i + EXTRA_ARGS] = strdup(arg.c_str());
  }

  // size
  struct winsize winp;
  winp.ws_col = info[4].As<Napi::Number>().Uint32Value();
  winp.ws_row = info[5].As<Napi::Number>().Uint32Value();
  winp.ws_xpixel = 0;
  winp.ws_ypixel = 0;

  // termios
  struct termios t = termios();
  struct termios *term = &t;
  term->c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
  if (info[9].As<Napi::Boolean>().Value()) {
#if defined(IUTF8)
    term->c_iflag |= IUTF8;
#endif
  }
  term->c_oflag = OPOST | ONLCR;
  term->c_cflag = CREAD | CS8 | HUPCL;
  term->c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;

  term->c_cc[VEOF] = 4;
  term->c_cc[VEOL] = -1;
  term->c_cc[VEOL2] = -1;
  term->c_cc[VERASE] = 0x7f;
  term->c_cc[VWERASE] = 23;
  term->c_cc[VKILL] = 21;
  term->c_cc[VREPRINT] = 18;
  term->c_cc[VINTR] = 3;
  term->c_cc[VQUIT] = 0x1c;
  term->c_cc[VSUSP] = 26;
  term->c_cc[VSTART] = 17;
  term->c_cc[VSTOP] = 19;
  term->c_cc[VLNEXT] = 22;
  term->c_cc[VDISCARD] = 15;
  term->c_cc[VMIN] = 1;
  term->c_cc[VTIME] = 0;

  #if (__APPLE__)
  term->c_cc[VDSUSP] = 25;
  term->c_cc[VSTATUS] = 20;
  #endif

  cfsetispeed(term, B38400);
  cfsetospeed(term, B38400);

  // helperPath
  std::string helper_path_ = info[11].As<Napi::String>().Utf8Value();
  char *helper_path = strdup(helper_path_.c_str());

  sigset_t newmask, oldmask;
  int flags = POSIX_SPAWN_USEVFORK;

  // temporarily block all signals
  // this is needed due to a race condition in openpty
  // and to avoid running signal handlers in the child
  // before exec* happened
  sigfillset(&newmask);
  pthread_sigmask(SIG_SETMASK, &newmask, &oldmask);

  int master, slave;
  int ret = pty_openpty(&master, &slave, nullptr, term, &winp);
  if (ret == -1) {
    perror("openpty failed");
    Napi::Error::New(env, "openpty failed.").ThrowAsJavaScriptException();

    goto done;
  }

  int comms_pipe[2];
  if (pipe(comms_pipe)) {
    perror("pipe() failed");
    Napi::Error::New(env, "pipe() failed.").ThrowAsJavaScriptException();

    goto done;
  }

  posix_spawn_file_actions_t acts;
  posix_spawn_file_actions_init(&acts);
  posix_spawn_file_actions_adddup2(&acts, slave, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&acts, slave, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&acts, slave, STDERR_FILENO);
  posix_spawn_file_actions_adddup2(&acts, comms_pipe[1], COMM_PIPE_FD);
  posix_spawn_file_actions_addclose(&acts, comms_pipe[1]);

  posix_spawnattr_t attrs;
  posix_spawnattr_init(&attrs);
  if (closeFDs) {
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
  }
  posix_spawnattr_setflags(&attrs, flags);

  { // suppresses "jump bypasses variable initialization" errors
    pid_t pid;
    auto error = posix_spawn(&pid, helper_path, &acts, &attrs, argv, envp);

    close(comms_pipe[1]);

    // reenable signals
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

    if (error) {
      throw_for_errno(env, "posix_spawn failed: ", error);
      goto done;
    }

    int helper_error[2];
    auto bytes_read = read(comms_pipe[0], &helper_error, sizeof(helper_error));
    close(comms_pipe[0]);

    if (bytes_read == sizeof(helper_error)) {
      if (helper_error[0] == COMM_ERR_EXEC) {
        throw_for_errno(env, "exec() failed: ", helper_error[1]);
      } else if (helper_error[0] == COMM_ERR_CHDIR) {
        throw_for_errno(env, "chdir() failed: ", helper_error[1]);
      } else if (helper_error[0] == COMM_ERR_SETUID) {
        throw_for_errno(env, "setuid() failed: ", helper_error[1]);
      } else if (helper_error[0] == COMM_ERR_SETGID) {
        throw_for_errno(env, "setgid() failed: ", helper_error[1]);
      }
      goto done;
    }

    if (pty_nonblock(master) == -1) {
      Napi::Error::New(env, "Could not set master fd to nonblocking.").ThrowAsJavaScriptException();

      goto done;
    }

    Napi::Object obj = Napi::Object::New(env);
    (obj).Set(Napi::String::New(env, "fd"),
      Napi::Number::New(env, master));
    (obj).Set(Napi::String::New(env, "pid"),
      Napi::Number::New(env, pid));
    (obj).Set(Napi::String::New(env, "pty"),
      Napi::String::New(env, ptsname(master)));

    pty_baton *baton = new pty_baton();
    baton->env = &env;
    baton->exit_code = 0;
    baton->signal_code = 0;
    baton->cb.Reset(info[10].As<Napi::Function>());
    baton->pid = pid;
    baton->async.data = baton;

    uv_async_init(uv_default_loop(), &baton->async, pty_after_waitpid);

    uv_thread_create(&baton->tid, pty_waitpid, static_cast<void*>(baton));

    return obj;
  }
done:
  posix_spawn_file_actions_destroy(&acts);
  posix_spawnattr_destroy(&attrs);

  if (argv) {
    for (int i = 0; i < argl; i++) free(argv[i]);
    delete[] argv;
  }
  if (envp) {
    for (int i = 0; i < envc; i++) free(envp[i]);
    delete[] envp;
  }
  free(helper_path);
}

Napi::Value PtyOpen(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() != 2 ||
      !info[0].IsNumber() ||
      !info[1].IsNumber()) {
    Napi::Error::New(env, "Usage: pty.open(cols, rows)").ThrowAsJavaScriptException();
    return env.Null();
  }

  // size
  struct winsize winp;
  winp.ws_col = info[0].As<Napi::Number>().Uint32Value();
  winp.ws_row = info[1].As<Napi::Number>().Uint32Value();
  winp.ws_xpixel = 0;
  winp.ws_ypixel = 0;

  // pty
  int master, slave;
  int ret = pty_openpty(&master, &slave, nullptr, NULL, &winp);

  if (ret == -1) {
    Napi::Error::New(env, "openpty(3) failed.").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (pty_nonblock(master) == -1) {
    Napi::Error::New(env, "Could not set master fd to nonblocking.").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (pty_nonblock(slave) == -1) {
    Napi::Error::New(env, "Could not set slave fd to nonblocking.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Object obj = Napi::Object::New(env);
  (obj).Set(Napi::String::New(env, "master"),
    Napi::Number::New(env, master));
  (obj).Set(Napi::String::New(env, "slave"),
    Napi::Number::New(env, slave));
  (obj).Set(Napi::String::New(env, "pty"),
    Napi::String::New(env, ptsname(master)));

  return obj;
}

Napi::Value PtyResize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() != 3 ||
      !info[0].IsNumber() ||
      !info[1].IsNumber() ||
      !info[2].IsNumber()) {
    Napi::Error::New(env, "Usage: pty.resize(fd, cols, rows)").ThrowAsJavaScriptException();
    return env.Null();
  }

  int fd = info[0].As<Napi::Number>();

  struct winsize winp;
  winp.ws_col = info[1].As<Napi::Number>().Uint32Value();
  winp.ws_row = info[2].As<Napi::Number>().Uint32Value();
  winp.ws_xpixel = 0;
  winp.ws_ypixel = 0;

  if (ioctl(fd, TIOCSWINSZ, &winp) == -1) {
    switch (errno) {
      case EBADF: Napi::Error::New(env, "ioctl(2) failed, EBADF").ThrowAsJavaScriptException();
 return env.Null();
      case EFAULT: Napi::Error::New(env, "ioctl(2) failed, EFAULT").ThrowAsJavaScriptException();
 return env.Null();
      case EINVAL: Napi::Error::New(env, "ioctl(2) failed, EINVAL").ThrowAsJavaScriptException();
 return env.Null();
      case ENOTTY: Napi::Error::New(env, "ioctl(2) failed, ENOTTY").ThrowAsJavaScriptException();
 return env.Null();
    }
    Napi::Error::New(env, "ioctl(2) failed").ThrowAsJavaScriptException();
    return env.Null();
  }

  return env.Undefined();
}

/**
 * Foreground Process Name
 */
Napi::Value PtyGetProc(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  if (info.Length() != 2 ||
      !info[0].IsNumber() ||
      !info[1].IsString()) {
    Napi::Error::New(env, "Usage: pty.process(fd, tty)").ThrowAsJavaScriptException();
    return env.Null();
  }

  int fd = info[0].As<Napi::Number>();

  std::string tty_ = info[1].As<Napi::String>().Utf8Value();
  char *tty = strdup(tty_.c_str());
  char *name = pty_getproc(fd, tty);
  free(tty);

  if (name == NULL) {
    return env.Undefined();
  }

  Napi::String name_ = Napi::String::New(env, name);
  free(name);
  return name_;
}

/**
 * Nonblocking FD
 */

static int
pty_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * pty_waitpid
 * Wait for SIGCHLD to read exit status.
 */

static void
pty_waitpid(void *data) {
  int ret;
  int stat_loc;

  pty_baton *baton = static_cast<pty_baton*>(data);

  errno = 0;

  if ((ret = waitpid(baton->pid, &stat_loc, 0)) != baton->pid) {
    if (ret == -1 && errno == EINTR) {
      return pty_waitpid(baton);
    }
    if (ret == -1 && errno == ECHILD) {
      // XXX node v0.8.x seems to have this problem.
      // waitpid is already handled elsewhere.
      ;
    } else {
      assert(false);
    }
  }

  if (WIFEXITED(stat_loc)) {
    baton->exit_code = WEXITSTATUS(stat_loc); // errno?
  }

  if (WIFSIGNALED(stat_loc)) {
    baton->signal_code = WTERMSIG(stat_loc);
  }

  uv_async_send(&baton->async);
}

/**
 * pty_after_waitpid
 * Callback after exit status has been read.
 */

static void
pty_after_waitpid(uv_async_t *async) {
  pty_baton *baton = static_cast<pty_baton*>(async->data);
  Napi::Env env = *baton->env;
  Napi::HandleScope scope(env);

  Napi::Value argv[] = {
    Napi::Number::New(env, baton->exit_code),
    Napi::Number::New(env, baton->signal_code),
  };

  Napi::Function cb = Napi::Function::New(env, baton->cb);
  baton->cb.Reset();
  memset(&baton->cb, -1, sizeof(baton->cb));
  Napi::AsyncResource resource("pty_after_waitpid");
  resource.runInAsyncScope(Napi::GetCurrentContext()->Global(), cb, 2, argv);

  uv_close((uv_handle_t *)async, pty_after_close);
}

/**
 * pty_after_close
 * uv_close() callback - free handle data
 */

static void
pty_after_close(uv_handle_t *handle) {
  uv_async_t *async = (uv_async_t *)handle;
  pty_baton *baton = static_cast<pty_baton*>(async->data);
  delete baton;
}

/**
 * pty_getproc
 * Taken from tmux.
 */

// Taken from: tmux (http://tmux.sourceforge.net/)
// Copyright (c) 2009 Nicholas Marriott <nicm@users.sourceforge.net>
// Copyright (c) 2009 Joshua Elsasser <josh@elsasser.org>
// Copyright (c) 2009 Todd Carson <toc@daybefore.net>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
// IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
// OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#if defined(__linux__)

static char *
pty_getproc(int fd, char *tty) {
  FILE *f;
  char *path, *buf;
  size_t len;
  int ch;
  pid_t pgrp;
  int r;

  if ((pgrp = tcgetpgrp(fd)) == -1) {
    return NULL;
  }

  r = asprintf(&path, "/proc/%lld/cmdline", (long long)pgrp);
  if (r == -1 || path == NULL) return NULL;

  if ((f = fopen(path, "r")) == NULL) {
    free(path);
    return NULL;
  }

  free(path);

  len = 0;
  buf = NULL;
  while ((ch = fgetc(f)) != EOF) {
    if (ch == '\0') break;
    buf = (char *)realloc(buf, len + 2);
    if (buf == NULL) return NULL;
    buf[len++] = ch;
  }

  if (buf != NULL) {
    buf[len] = '\0';
  }

  fclose(f);
  return buf;
}

#elif defined(__APPLE__)

static char *
pty_getproc(int fd, char *tty) {
  int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, 0 };
  size_t size;
  struct kinfo_proc kp;

  if ((mib[3] = tcgetpgrp(fd)) == -1) {
    return NULL;
  }

  size = sizeof kp;
  if (sysctl(mib, 4, &kp, &size, NULL, 0) == -1) {
    return NULL;
  }

  if (size != (sizeof kp) || *kp.kp_proc.p_comm == '\0') {
    return NULL;
  }

  return strdup(kp.kp_proc.p_comm);
}

#else

static char *
pty_getproc(int fd, char *tty) {
  return NULL;
}

#endif

/**
 * openpty(3) / forkpty(3)
 */

static int
pty_openpty(int *amaster,
            int *aslave,
            char *name,
            const struct termios *termp,
            const struct winsize *winp) {
#if defined(__sun)
  char *slave_name;
  int slave;
  int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
  if (master == -1) return -1;
  if (amaster) *amaster = master;

  if (grantpt(master) == -1) goto err;
  if (unlockpt(master) == -1) goto err;

  slave_name = ptsname(master);
  if (slave_name == NULL) goto err;
  if (name) strcpy(name, slave_name);

  slave = open(slave_name, O_RDWR | O_NOCTTY);
  if (slave == -1) goto err;
  if (aslave) *aslave = slave;

  ioctl(slave, I_PUSH, "ptem");
  ioctl(slave, I_PUSH, "ldterm");
  ioctl(slave, I_PUSH, "ttcompat");

  if (termp) tcsetattr(slave, TCSAFLUSH, termp);
  if (winp) ioctl(slave, TIOCSWINSZ, winp);

  return 0;

err:
  close(master);
  return -1;
#else
  return openpty(amaster, aslave, name, (termios *)termp, (winsize *)winp);
#endif
}

/**
 * Init
 */

Napi::Object init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);
  exports.Set(Napi::String::New(env, "fork"), Napi::Function::New(env, PtyFork));
  exports.Set(Napi::String::New(env, "open"), Napi::Function::New(env, PtyOpen));
  exports.Set(Napi::String::New(env, "resize"), Napi::Function::New(env, PtyResize));
  exports.Set(Napi::String::New(env, "process"), Napi::Function::New(env, PtyGetProc));
  return exports;
}

NODE_API_MODULE(pty, init)
