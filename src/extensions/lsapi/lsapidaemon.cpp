/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2022  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include "lsapidaemon.h"

#include <extensions/localworkerconfig.h>
#include <extensions/extworker.h>
#include <extensions/registry/extappregistry.h>
#include <log4cxx/logger.h>
#include <lsr/ls_fileio.h>
#include <main/mainserverconfig.h>
#include <util/env.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Static registry
// ---------------------------------------------------------------------------

GHash *LsapiDaemon::s_pDaemons = NULL;

GHash *LsapiDaemon::ensureRegistry()
{
    if (!s_pDaemons)
    {
        s_pDaemons = new GHash(17, GHash::hfString,
                                GHash::cmpString);
    }
    return s_pDaemons;
}


// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LsapiDaemon::LsapiDaemon(const char *pUsername, uid_t uid, gid_t gid)
    : m_sUsername(pUsername)
    , m_uid(uid)
    , m_gid(gid)
    , m_pidDaemon(-1)
    , m_fdListen(-1)
    , m_iActiveChildren(0)
    , m_tmStarted(0)
{
    buildSocketPath();
}


LsapiDaemon::~LsapiDaemon()
{
    stop();
}


// ---------------------------------------------------------------------------
// Static accessors
// ---------------------------------------------------------------------------

LsapiDaemon *LsapiDaemon::getDaemon(const char *pUsername,
                                      uid_t uid, gid_t gid)
{
    GHash *pReg = ensureRegistry();
    GHash::iterator it = pReg->find(pUsername);
    if (it != pReg->end())
        return (LsapiDaemon *)it->second();

    LsapiDaemon *pDaemon = new LsapiDaemon(pUsername, uid, gid);
    pReg->insert(pDaemon->m_sUsername.c_str(), pDaemon);
    return pDaemon;
}


void LsapiDaemon::removeDaemon(const char *pUsername)
{
    if (!s_pDaemons)
        return;
    GHash::iterator it = s_pDaemons->find(pUsername);
    if (it != s_pDaemons->end())
    {
        LsapiDaemon *pDaemon = (LsapiDaemon *)it->second();
        s_pDaemons->erase(it);
        delete pDaemon;
    }
}


void LsapiDaemon::shutdownAll()
{
    if (!s_pDaemons)
        return;

    GHash::iterator it;
    for (it = s_pDaemons->begin(); it != s_pDaemons->end();
         it = s_pDaemons->next(it))
    {
        LsapiDaemon *pDaemon = (LsapiDaemon *)it->second();
        if (pDaemon)
        {
            pDaemon->stop();
            delete pDaemon;
        }
    }
    s_pDaemons->clear();
    delete s_pDaemons;
    s_pDaemons = NULL;
}


// ---------------------------------------------------------------------------
// Socket path
// ---------------------------------------------------------------------------

void LsapiDaemon::buildSocketPath()
{
    char achBuf[512];
    snprintf(achBuf, sizeof(achBuf),
             LSAPI_DAEMON_SOCK_DIR "lsapi_daemon_%s.sock",
             m_sUsername.c_str());
    m_sSocketPath.setStr(achBuf);
}


// ---------------------------------------------------------------------------
// Unix domain socket creation
// ---------------------------------------------------------------------------

int LsapiDaemon::createListenSocket()
{
    struct sockaddr_un addr;
    int fd;

    // Ensure directory exists
    mkdir(LSAPI_DAEMON_SOCK_DIR, 0755);

    // Remove stale socket
    unlink(m_sSocketPath.c_str());

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
    {
        LS_ERROR("[LsapiDaemon] Failed to create socket for user [%s]: %s",
                 m_sUsername.c_str(), strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
             m_sSocketPath.c_str());

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        LS_ERROR("[LsapiDaemon] Failed to bind socket [%s] for user [%s]: %s",
                 m_sSocketPath.c_str(), m_sUsername.c_str(), strerror(errno));
        close(fd);
        return -1;
    }

    // Set ownership so the daemon process (running as target user) can accept
    if (chown(m_sSocketPath.c_str(), m_uid, m_gid) == -1)
    {
        LS_WARN("[LsapiDaemon] chown socket [%s] failed: %s",
                m_sSocketPath.c_str(), strerror(errno));
    }
    chmod(m_sSocketPath.c_str(), 0660);

    if (listen(fd, m_config.m_iMaxChildren + 4) == -1)
    {
        LS_ERROR("[LsapiDaemon] Failed to listen on socket [%s]: %s",
                 m_sSocketPath.c_str(), strerror(errno));
        close(fd);
        unlink(m_sSocketPath.c_str());
        return -1;
    }

    // Set close-on-exec for the server-side copy
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    return fd;
}


// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

int LsapiDaemon::isRunning() const
{
    if (m_pidDaemon <= 0)
        return 0;
    if (kill(m_pidDaemon, 0) == 0)
        return 1;
    if (errno == EPERM)
        return 1;   // alive but we lack permission (shouldn't happen)
    return 0;
}


int LsapiDaemon::start(LocalWorkerConfig *pConfig, ExtWorker *pWorker)
{
    if (m_config.m_iDaemonMode == LSAPI_DAEMON_OFF)
        return -1;

    // Already running?
    if (isRunning())
    {
        LS_DBG_L("[LsapiDaemon] Daemon for user [%s] already running, "
                 "pid %d", m_sUsername.c_str(), m_pidDaemon);
        return m_pidDaemon;
    }

    // Clean up any stale state
    if (m_fdListen >= 0)
    {
        close(m_fdListen);
        m_fdListen = -1;
    }
    removeSocket();

    int listenFd = createListenSocket();
    if (listenFd < 0)
        return -1;

    m_fdListen = listenFd;
    int pid = forkDaemonParent(pConfig, pWorker, listenFd);
    if (pid > 0)
    {
        m_pidDaemon = pid;
        m_tmStarted = time(NULL);
        m_iActiveChildren = 0;

        LS_NOTICE("[LsapiDaemon] Started daemon for user [%s], "
                  "pid %d, socket [%s], mode %d, maxChildren %d",
                  m_sUsername.c_str(), pid, m_sSocketPath.c_str(),
                  m_config.m_iDaemonMode, m_config.m_iMaxChildren);

        // Register pid so the server can track it
        PidRegistry::add(pid, pWorker, 0);
    }
    else
    {
        LS_ERROR("[LsapiDaemon] Failed to fork daemon for user [%s]",
                 m_sUsername.c_str());
        close(m_fdListen);
        m_fdListen = -1;
        removeSocket();
    }

    return pid;
}


int LsapiDaemon::stop()
{
    if (m_pidDaemon > 0)
    {
        LS_NOTICE("[LsapiDaemon] Stopping daemon for user [%s], pid %d",
                  m_sUsername.c_str(), m_pidDaemon);
        killDaemon();
        PidRegistry::remove(m_pidDaemon);
        m_pidDaemon = -1;
    }

    if (m_fdListen >= 0)
    {
        close(m_fdListen);
        m_fdListen = -1;
    }

    removeSocket();
    m_iActiveChildren = 0;
    return 0;
}


void LsapiDaemon::killDaemon()
{
    if (m_pidDaemon <= 0)
        return;

    // In processgroup mode, kill the entire process group
    if (m_config.m_iDaemonMode == LSAPI_DAEMON_PROCESSGROUP)
    {
        kill(-m_pidDaemon, SIGTERM);
    }
    else
    {
        kill(m_pidDaemon, SIGTERM);
    }

    // Brief wait for graceful shutdown
    int waited = 0;
    while (waited < 5)
    {
        int status;
        pid_t ret = waitpid(m_pidDaemon, &status, WNOHANG);
        if (ret == m_pidDaemon || (ret == -1 && errno == ECHILD))
            return;
        usleep(200000); // 200ms
        waited++;
    }

    // Force kill
    if (m_config.m_iDaemonMode == LSAPI_DAEMON_PROCESSGROUP)
        kill(-m_pidDaemon, SIGKILL);
    else
        kill(m_pidDaemon, SIGKILL);

    waitpid(m_pidDaemon, NULL, WNOHANG);
}


void LsapiDaemon::removeSocket()
{
    if (m_sSocketPath.c_str() && m_sSocketPath.c_str()[0])
        unlink(m_sSocketPath.c_str());
}


// ---------------------------------------------------------------------------
// Timer callback: health-check the daemon, reap zombies
// ---------------------------------------------------------------------------

void LsapiDaemon::onTimer()
{
    if (m_pidDaemon <= 0)
        return;

    // Check if daemon is still alive
    if (!isRunning())
    {
        LS_WARN("[LsapiDaemon] Daemon for user [%s] pid %d has died",
                m_sUsername.c_str(), m_pidDaemon);
        PidRegistry::remove(m_pidDaemon);
        m_pidDaemon = -1;
        m_iActiveChildren = 0;
        // Will be restarted on next request via startEx()
        return;
    }

    // Reap any zombie children of the daemon's process group
    // (The daemon parent itself handles child reaping, but the server
    //  should also collect the daemon process if it exits.)
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        if (pid == m_pidDaemon)
        {
            LS_NOTICE("[LsapiDaemon] Daemon parent [%s] pid %d exited, "
                      "status %d", m_sUsername.c_str(), pid, status);
            PidRegistry::remove(m_pidDaemon);
            m_pidDaemon = -1;
            m_iActiveChildren = 0;
            break;
        }
        // Other children are handled by PidRegistry
    }
}


// ---------------------------------------------------------------------------
// Fork the persistent daemon parent
// ---------------------------------------------------------------------------
//
// The forked daemon parent process:
//   1. Drops privileges to target uid/gid (suEXEC)
//   2. Enters a loop: accept() on the Unix socket
//   3. Forks a child LSPHP process for each accepted connection
//   4. Tracks children, enforces limits, reaps exited children
//   5. In PROCESSGROUP mode, all children share the parent's pgid
//
// The child inherits the accepted connection fd as LSAPI_SOCK_FILENO (fd 0),
// then exec()s the LSPHP binary, which speaks LSAPI over that fd.
// ---------------------------------------------------------------------------

int LsapiDaemon::forkDaemonParent(LocalWorkerConfig *pConfig,
                                   ExtWorker *pWorker,
                                   int listenFd)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        LS_ERROR("[LsapiDaemon] fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid > 0)
    {
        // Parent (web server) process: return the daemon pid
        return pid;
    }

    // === Child process: this becomes the persistent daemon parent ===

    // In processgroup mode, become a new process group leader
    if (m_config.m_iDaemonMode == LSAPI_DAEMON_PROCESSGROUP)
        setpgid(0, 0);

    // Drop privileges
    if (m_gid > 0 && setgid(m_gid) == -1)
    {
        fprintf(stderr, "[LsapiDaemon] setgid(%d) failed: %s\n",
                m_gid, strerror(errno));
        _exit(1);
    }
    if (m_uid > 0 && setuid(m_uid) == -1)
    {
        fprintf(stderr, "[LsapiDaemon] setuid(%d) failed: %s\n",
                m_uid, strerror(errno));
        _exit(1);
    }

    // Set environment variables for LSAPI children
    const char *pCommand = pConfig->getCommand();
    if (!pCommand || !pCommand[0])
    {
        fprintf(stderr, "[LsapiDaemon] No command configured\n");
        _exit(1);
    }

    // Set LSAPI env vars so children know the limits
    char achBuf[64];
    snprintf(achBuf, sizeof(achBuf), "%d", m_config.m_iMaxChildren);
    setenv("LSAPI_CHILDREN", achBuf, 1);

    snprintf(achBuf, sizeof(achBuf), "%d", m_config.m_iMaxReqs);
    setenv("LSAPI_MAX_REQS", achBuf, 1);

    snprintf(achBuf, sizeof(achBuf), "%d", m_config.m_iMaxIdleTime);
    setenv("LSAPI_MAX_IDLE", achBuf, 1);

    snprintf(achBuf, sizeof(achBuf), "%d", m_config.m_iMaxChildren);
    setenv("LSAPI_PGRP_MAX_IDLE", achBuf, 1);

    setenv("LSAPI_PPID_NO_CHECK", "1", 1);

    // Set up signal handlers
    signal(SIGPIPE, SIG_IGN);

    // Track child PIDs
    struct ChildSlot
    {
        pid_t   pid;
        time_t  startTime;
        time_t  lastActive;
        int     reqCount;
    };

    int maxCh = m_config.m_iMaxChildren;
    ChildSlot *children = (ChildSlot *)calloc(maxCh, sizeof(ChildSlot));
    if (!children)
        _exit(1);
    int activeCount = 0;

    // Set non-blocking accept with timeout via select()
    // Main daemon loop
    volatile int running = 1;

    // Handle SIGTERM gracefully
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int) { /* will cause accept/select to return EINTR */ };
    sa.sa_flags = 0;    // no SA_RESTART so syscalls get interrupted
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Install SIGCHLD handler to reap children
    signal(SIGCHLD, SIG_DFL);

    while (running)
    {
        // Reap any exited children first
        int status;
        pid_t exited;
        while ((exited = waitpid(-1, &status, WNOHANG)) > 0)
        {
            for (int i = 0; i < maxCh; i++)
            {
                if (children[i].pid == exited)
                {
                    children[i].pid = 0;
                    activeCount--;
                    break;
                }
            }
        }

        // Check for idle children that exceeded max idle time
        time_t now = time(NULL);
        for (int i = 0; i < maxCh; i++)
        {
            if (children[i].pid > 0 &&
                children[i].lastActive > 0 &&
                (now - children[i].lastActive) > m_config.m_iMaxIdleTime)
            {
                kill(children[i].pid, SIGTERM);
            }
        }

        // Wait for a connection with a timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenFd, &readfds);
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int ret = select(listenFd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                // Check if we should exit
                // Re-check after signal
                continue;
            }
            break;  // fatal select error
        }

        if (ret == 0)
            continue;   // timeout, loop back to reap/check

        // Accept the connection
        int connFd = accept(listenFd, NULL, NULL);
        if (connFd < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            // Transient error, keep going
            continue;
        }

        // Check if we have room for another child
        if (activeCount >= maxCh)
        {
            // At capacity: close the connection, web server will retry
            close(connFd);
            continue;
        }

        // Find an empty child slot
        int slot = -1;
        for (int i = 0; i < maxCh; i++)
        {
            if (children[i].pid <= 0)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
        {
            close(connFd);
            continue;
        }

        // Fork a child to handle this connection
        pid_t childPid = fork();
        if (childPid < 0)
        {
            close(connFd);
            continue;
        }

        if (childPid == 0)
        {
            // === Child LSPHP worker process ===
            // Close the listen socket (child doesn't need it)
            close(listenFd);

            // Redirect the connection fd to LSAPI_SOCK_FILENO (fd 0)
            if (connFd != 0)
            {
                dup2(connFd, 0);
                close(connFd);
            }

            // Close other fds > 2 (stdin=0 is our LSAPI socket)
            for (int f = 3; f < 1024; f++)
                close(f);

            // Build argv from command
            char cmdBuf[4096];
            strncpy(cmdBuf, pCommand, sizeof(cmdBuf) - 1);
            cmdBuf[sizeof(cmdBuf) - 1] = '\0';

            char *argv[256];
            int argc = 0;
            char *p = cmdBuf;
            while (*p && argc < 255)
            {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (!*p)
                    break;
                argv[argc++] = p;
                while (*p && *p != ' ' && *p != '\t')
                    p++;
                if (*p)
                    *p++ = '\0';
            }
            argv[argc] = NULL;

            if (argc > 0)
            {
                execv(argv[0], argv);
                // If execv fails, try execvp
                execvp(argv[0], argv);
            }

            fprintf(stderr, "[LsapiDaemon] execv(%s) failed: %s\n",
                    pCommand, strerror(errno));
            _exit(127);
        }

        // === Daemon parent continues ===
        close(connFd);  // parent doesn't need the connection fd

        children[slot].pid = childPid;
        children[slot].startTime = now;
        children[slot].lastActive = now;
        children[slot].reqCount = 0;
        activeCount++;

        // In processgroup mode, the child inherits our pgid automatically
    }

    // Cleanup: kill all children
    for (int i = 0; i < maxCh; i++)
    {
        if (children[i].pid > 0)
        {
            kill(children[i].pid, SIGTERM);
        }
    }

    // Wait briefly for children to exit
    usleep(500000);
    for (int i = 0; i < maxCh; i++)
    {
        if (children[i].pid > 0)
        {
            kill(children[i].pid, SIGKILL);
            waitpid(children[i].pid, NULL, 0);
        }
    }

    free(children);
    close(listenFd);
    _exit(0);

    return -1; // unreachable, for compiler
}
