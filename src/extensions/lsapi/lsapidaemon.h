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
#ifndef LSAPIDAEMON_H
#define LSAPIDAEMON_H

#include <lsdef.h>
#include <extensions/pidlist.h>
#include <util/autostr.h>
#include <util/ghash.h>

#include <sys/types.h>
#include <time.h>

class LocalWorkerConfig;
class ExtWorker;

// Daemon mode values for lsapiDaemonMode config
#define LSAPI_DAEMON_OFF            0
#define LSAPI_DAEMON_DAEMON         1
#define LSAPI_DAEMON_PROCESSGROUP   2

// Default config values
#define LSAPI_DAEMON_MAX_CHILDREN       100
#define LSAPI_DAEMON_MAX_IDLE_TIME      300
#define LSAPI_DAEMON_MAX_REQS           10000
#define LSAPI_DAEMON_SOCK_DIR           "/tmp/lshttpd/"

/**
 * LsapiDaemonConfig: configuration for a daemon/processgroup instance.
 */
struct LsapiDaemonConfig
{
    int     m_iDaemonMode;      // 0=off, 1=daemon, 2=processgroup
    int     m_iMaxChildren;     // max child LSPHP workers per user
    int     m_iMaxIdleTime;     // seconds before idle child is reaped
    int     m_iMaxReqs;         // max requests before child is recycled

    LsapiDaemonConfig()
        : m_iDaemonMode(LSAPI_DAEMON_OFF)
        , m_iMaxChildren(LSAPI_DAEMON_MAX_CHILDREN)
        , m_iMaxIdleTime(LSAPI_DAEMON_MAX_IDLE_TIME)
        , m_iMaxReqs(LSAPI_DAEMON_MAX_REQS)
    {}
};


/**
 * LsapiDaemonChild: tracks one forked LSPHP child process.
 */
struct LsapiDaemonChild
{
    pid_t   m_pid;
    time_t  m_tmStart;
    time_t  m_tmLastActive;
    int     m_iReqsHandled;
    int     m_iIdle;            // 1 if currently idle

    LsapiDaemonChild()
        : m_pid(-1)
        , m_tmStart(0)
        , m_tmLastActive(0)
        , m_iReqsHandled(0)
        , m_iIdle(1)
    {}
};


/**
 * LsapiDaemon: manages a persistent parent LSPHP process per user.
 *
 * In daemon mode, one parent LSPHP process is spawned per user, listening
 * on a Unix domain socket. When requests arrive, the parent forks child
 * processes to handle them, avoiding per-request process creation overhead.
 *
 * In processgroup mode, the behavior is similar but children share the
 * parent's process group for coordinated signal handling.
 */
class LsapiDaemon
{
public:
    /**
     * Get the singleton instance keyed by username.
     * Each user gets exactly one LsapiDaemon.
     */
    static LsapiDaemon *getDaemon(const char *pUsername, uid_t uid, gid_t gid);

    /**
     * Remove and clean up the daemon for a given user.
     */
    static void removeDaemon(const char *pUsername);

    /**
     * Clean up all daemons (called at server shutdown).
     */
    static void shutdownAll();

    /**
     * Start the daemon parent process if not already running.
     * Sets up the Unix socket and forks the persistent parent.
     *
     * @param pConfig   the worker config (for command, env, etc.)
     * @param pWorker   the ExtWorker that owns this daemon
     * @return daemon PID on success, -1 on failure
     */
    int start(LocalWorkerConfig *pConfig, ExtWorker *pWorker);

    /**
     * Stop the daemon parent and all its children.
     */
    int stop();

    /**
     * Check if the daemon parent process is still alive.
     */
    int isRunning() const;

    /**
     * Called periodically (every ~10s) to reap idle children
     * and check daemon health.
     */
    void onTimer();

    /**
     * Get the Unix socket path for this daemon.
     */
    const char *getSocketPath() const
    {   return m_sSocketPath.c_str();   }

    /**
     * Get the daemon parent PID.
     */
    pid_t getDaemonPid() const
    {   return m_pidDaemon;             }

    /**
     * Get the number of active child processes.
     */
    int getActiveChildren() const
    {   return m_iActiveChildren;       }

    /**
     * Get the daemon config.
     */
    LsapiDaemonConfig &getConfig()
    {   return m_config;                }
    const LsapiDaemonConfig &getConfig() const
    {   return m_config;                }

    const char *getUsername() const
    {   return m_sUsername.c_str();      }

    uid_t getUid() const    {   return m_uid;   }
    gid_t getGid() const    {   return m_gid;   }

private:
    LsapiDaemon(const char *pUsername, uid_t uid, gid_t gid);
    ~LsapiDaemon();

    /**
     * Build the socket path: /tmp/lshttpd/lsapi_daemon_{username}.sock
     */
    void buildSocketPath();

    /**
     * Create and bind the Unix domain socket.
     * @return fd on success, -1 on failure
     */
    int createListenSocket();

    /**
     * Fork the persistent daemon parent process.
     * The parent will:
     * 1. setuid/setgid to the target user
     * 2. Listen on the Unix socket
     * 3. Fork children on incoming connections
     * 4. Reap exited children
     * 5. Enforce max children, idle timeout, max requests
     */
    int forkDaemonParent(LocalWorkerConfig *pConfig, ExtWorker *pWorker,
                         int listenFd);

    /**
     * Send SIGTERM to daemon, wait briefly, then SIGKILL if needed.
     */
    void killDaemon();

    /**
     * Remove the Unix socket file from disk.
     */
    void removeSocket();

    // Per-daemon state
    AutoStr         m_sUsername;
    uid_t           m_uid;
    gid_t           m_gid;
    pid_t           m_pidDaemon;        // PID of the persistent parent
    int             m_fdListen;         // listen socket fd (server side)
    int             m_iActiveChildren;
    time_t          m_tmStarted;
    AutoStr         m_sSocketPath;
    LsapiDaemonConfig m_config;

    static GHash *ensureRegistry();

    // Global registry: username -> LsapiDaemon*
    static GHash   *s_pDaemons;

    LS_NO_COPY_ASSIGN(LsapiDaemon);
};


#endif // LSAPIDAEMON_H
