/*
    minit - minimalist init implementation for containers
            <https://github.com/chazomaticus/minit>
    Copyright 2014 Charles Lindsay <chaz@chazomatic.us>

    This software is provided 'as-is', without any express or implied warranty.
    In no event will the authors be held liable for any damages arising from
    the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software in
       a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
*/

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef DEFAULT_STARTUP
#define DEFAULT_STARTUP "/etc/minit/startup"
#endif
#ifndef DEFAULT_SHUTDOWN
#define DEFAULT_SHUTDOWN "/etc/minit/shutdown"
#endif
#ifndef DEFAULT_RELOAD
#define DEFAULT_RELOAD "/etc/minit/reload"
#endif


static const char *const default_startup = DEFAULT_STARTUP;
static const char *const default_shutdown = DEFAULT_SHUTDOWN;
static const char *const default_reload = DEFAULT_RELOAD;

static volatile pid_t shutdown_pid = 0;
static volatile pid_t reload_pid = 0;

static volatile int terminate = 0;
static volatile int reload = 0;


static void handle_child(int sig) {
    int saved_errno = errno;
    pid_t pid;

    for(; (pid = waitpid(-1, NULL, WNOHANG)) > 0; ) {
        if(pid == shutdown_pid)
            shutdown_pid = 0;
        if(pid == reload_pid)
            reload_pid = 0;
    }

    if(pid == -1)
        terminate = 1;

    errno = saved_errno;
}

static void handle_termination(int sig) {
    terminate = 1;
}

static void handle_reload(int sig) {
    reload = 1;
}

static sigset_t setup_signals(sigset_t *out_default_mask) {
    sigset_t all_mask;
    sigfillset(&all_mask);
    sigprocmask(SIG_SETMASK, &all_mask, out_default_mask);

    sigset_t suspend_mask;
    sigfillset(&suspend_mask);

    struct sigaction action = { .sa_flags = SA_NOCLDSTOP | SA_RESTART };
    sigfillset(&action.sa_mask);

    action.sa_handler = handle_child;
    sigaction(SIGCHLD, &action, NULL);
    sigdelset(&suspend_mask, SIGCHLD);

    action.sa_handler = handle_termination;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigdelset(&suspend_mask, SIGTERM);
    sigdelset(&suspend_mask, SIGINT);

    // TODO: also handle SIGUSR1/2, maybe run another script/s?
    action.sa_handler = handle_reload;
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigdelset(&suspend_mask, SIGUSR1);
    sigdelset(&suspend_mask, SIGHUP);

    return suspend_mask;
}

static pid_t run(const char *filename, sigset_t child_mask) {
    pid_t pid = fork();
    if(pid == -1)
        perror("minit: fork");

    if(pid == 0) {
        sigprocmask(SIG_SETMASK, &child_mask, NULL);
        execlp(filename, filename, NULL);

        // Ignore "no such file" errors unless specified by caller.
        if((filename == default_startup || filename == default_shutdown || filename == default_reload)
                && errno == ENOENT)
            exit(0);

        perror(filename);
        exit(1);
    }

    return pid;
}

int main(int argc, char *argv[]) {
    sigset_t default_mask;
    sigset_t suspend_mask = setup_signals(&default_mask);

    const char *startup_script = (argc > 1 && *argv[1] ? argv[1] : default_startup);
    const char *shutdown_script = (argc > 2 && *argv[2] ? argv[2] : default_shutdown);
    const char *reload_script = (argc > 3 && *argv[3] ? argv[3] : default_reload);

    run(startup_script, default_mask);

run_forever:
    while(!(terminate || reload))
        sigsuspend(&suspend_mask);

    if(reload) {
        reload_pid = run(reload_script, default_mask);
        while(reload_pid > 0)
            sigsuspend(&suspend_mask);

        reload = 0;
        goto run_forever;
    }

    shutdown_pid = run(shutdown_script, default_mask);
    while(shutdown_pid > 0)
        sigsuspend(&suspend_mask);

    // If we're running as a regular process (not init), don't kill -1.
    if(getpid() == 1) {
        kill(-1, SIGTERM);
        while(wait(NULL) > 0)
            continue;
    }

    return 0;
}
