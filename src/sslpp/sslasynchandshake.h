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

#ifndef SSLASYNCHANDSHAKE_H
#define SSLASYNCHANDSHAKE_H

#include <lsdef.h>
#include <lsr/ls_node.h>
#include <lsr/ls_atomic.h>

typedef struct ls_lfqueue_s ls_lfqueue_t;

class SslConnection;
class NtwkIOLink;
class WorkCrew;
class EventNotifier;

/**
 * @class SslAsyncHandshake
 * @brief Offloads SSL/TLS handshake to worker threads to avoid blocking
 *        the main event loop during CPU-intensive cryptographic operations.
 *
 * When enabled via `sslAsyncHandshake 1` in the config, incoming SSL
 * connections queue their handshake to a thread pool. Once complete,
 * the event loop is notified via EventNotifier and the connection
 * resumes normal processing.
 *
 * The job struct embeds ls_lfnodei_t so it can be used with the
 * lock-free queue that WorkCrew expects.
 */

struct SslHandshakeJob
{
    ls_lfnodei_t    m_node;         // Must be first for WorkCrew queue
    NtwkIOLink     *m_pIOLink;      // The connection to handshake
    SslConnection  *m_pSslConn;     // SSL connection object
    int             m_iResult;      // Result: 1=success, 0=want_retry, -1=error
    int             m_iErrno;       // Preserved errno from worker thread
};


class SslAsyncHandshake
{
public:
    /**
     * @brief Initialize the async handshake subsystem.
     * @param numWorkers  Number of worker threads for handshake offloading.
     * @return 0 on success, -1 on failure.
     */
    static int  init(int numWorkers);

    /**
     * @brief Shut down the async handshake subsystem and stop workers.
     */
    static void shutdown();

    /**
     * @brief Queue an SSL handshake to be performed asynchronously.
     * @param pIOLink   The NtwkIOLink that owns the connection.
     * @param pSslConn  The SslConnection to handshake.
     * @return 0 if queued successfully, -1 on error (caller should
     *         fall back to synchronous handshake).
     */
    static int  queueHandshake(NtwkIOLink *pIOLink, SslConnection *pSslConn);

    /**
     * @brief Called from the event loop when async handshakes complete.
     *        Processes finished jobs and resumes connections.
     */
    static void processCompletedJobs();

    /**
     * @brief Check if async handshake is enabled and initialized.
     */
    static bool isEnabled()    {   return s_iEnabled != 0;     }

    /**
     * @brief Enable or disable async handshake offloading.
     */
    static void setEnabled(int v)  {   s_iEnabled = v;         }

private:
    /**
     * @brief Worker thread processing function for WorkCrew.
     *        Performs the actual SSL_do_handshake() call.
     */
    static void *handshakeWorkerFn(ls_lfnodei_t *item);

    /**
     * @brief Allocate a job from the pool or heap.
     */
    static SslHandshakeJob *allocJob();

    /**
     * @brief Return a job to the pool.
     */
    static void freeJob(SslHandshakeJob *pJob);

    static WorkCrew        *s_pWorkCrew;
    static ls_lfqueue_t    *s_pFinishedQueue;
    static volatile int     s_iEnabled;

    LS_NO_COPY_ASSIGN(SslAsyncHandshake);
};

#endif // SSLASYNCHANDSHAKE_H
