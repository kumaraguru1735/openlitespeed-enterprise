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

#include "sslasynchandshake.h"
#include <sslpp/sslconnection.h>
#include <http/ntwkiolink.h>
#include <thread/workcrew.h>
#include <edio/evtcbque.h>
#include <edio/multiplexer.h>
#include <edio/multiplexerfactory.h>
#include <log4cxx/logger.h>
#include <lsr/ls_lfqueue.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

WorkCrew       *SslAsyncHandshake::s_pWorkCrew = NULL;
ls_lfqueue_t   *SslAsyncHandshake::s_pFinishedQueue = NULL;
int             SslAsyncHandshake::s_iEnabled = 0;


/**
 * Event loop callback invoked when an async handshake has completed.
 * Runs on the main event loop thread so it is safe to access
 * NtwkIOLink and the multiplexer.
 *
 * lParam encodes the handshake result:
 *   1 = handshake succeeded
 *   0 = want read/write (need I/O then retry)
 *  -1 = fatal error
 */
static int asyncHandshakeDoneCb(evtcbhead_t *pSession,
                                long lParam, void *pParam)
{
    NtwkIOLink *pIOLink = static_cast<NtwkIOLink *>(
        static_cast<LsiSession *>(pSession));
    (void)pParam;

    if (!pIOLink)
        return 0;

    if (lParam == 1)
    {
        // Handshake succeeded -- resume the connection via SSLAgain()
        // which will detect CONNECTED status and set up the handler.
        MultiplexerFactory::getMultiplexer()->continueRead(pIOLink);
    }
    else if (lParam == 0)
    {
        // Needs more I/O -- resume read/write so the event loop retries.
        MultiplexerFactory::getMultiplexer()->continueRead(pIOLink);
        MultiplexerFactory::getMultiplexer()->continueWrite(pIOLink);
    }
    else
    {
        // Fatal error during handshake.
        LS_DBG_L(pIOLink, "SslAsyncHandshake: handshake failed, closing.");
        pIOLink->close();
    }
    return 0;
}


int SslAsyncHandshake::init(int numWorkers)
{
    if (s_pWorkCrew)
        return 0;  // Already initialized

    if (numWorkers <= 0)
        numWorkers = 2;
    if (numWorkers > 16)
        numWorkers = 16;

    s_pFinishedQueue = ls_lfqueue_new();
    if (!s_pFinishedQueue)
    {
        LS_ERROR("SslAsyncHandshake: failed to create finished queue.");
        return -1;
    }

    s_pWorkCrew = new WorkCrew(numWorkers, handshakeWorkerFn,
                               s_pFinishedQueue, NULL);
    if (!s_pWorkCrew)
    {
        ls_lfqueue_delete(s_pFinishedQueue);
        s_pFinishedQueue = NULL;
        LS_ERROR("SslAsyncHandshake: failed to create WorkCrew.");
        return -1;
    }

    int ret = s_pWorkCrew->startProcessing();
    if (ret != 0)
    {
        LS_ERROR("SslAsyncHandshake: failed to start worker threads.");
        delete s_pWorkCrew;
        s_pWorkCrew = NULL;
        ls_lfqueue_delete(s_pFinishedQueue);
        s_pFinishedQueue = NULL;
        return -1;
    }

    s_iEnabled = 1;
    LS_NOTICE("SslAsyncHandshake: initialized with %d worker threads.",
              numWorkers);
    return 0;
}


void SslAsyncHandshake::shutdown()
{
    if (s_pWorkCrew)
    {
        s_pWorkCrew->stopProcessing();
        delete s_pWorkCrew;
        s_pWorkCrew = NULL;
    }
    if (s_pFinishedQueue)
    {
        ls_lfnodei_t *pNode;
        while ((pNode = ls_lfqueue_get(s_pFinishedQueue)) != NULL)
            freeJob((SslHandshakeJob *)pNode);
        ls_lfqueue_delete(s_pFinishedQueue);
        s_pFinishedQueue = NULL;
    }
    s_iEnabled = 0;
    LS_NOTICE("SslAsyncHandshake: shutdown complete.");
}


SslHandshakeJob *SslAsyncHandshake::allocJob()
{
    SslHandshakeJob *pJob = (SslHandshakeJob *)malloc(sizeof(SslHandshakeJob));
    if (pJob)
        memset(pJob, 0, sizeof(SslHandshakeJob));
    return pJob;
}


void SslAsyncHandshake::freeJob(SslHandshakeJob *pJob)
{
    free(pJob);
}


int SslAsyncHandshake::queueHandshake(NtwkIOLink *pIOLink,
                                       SslConnection *pSslConn)
{
    if (!s_pWorkCrew || !s_iEnabled)
        return -1;

    SslHandshakeJob *pJob = allocJob();
    if (!pJob)
    {
        LS_ERROR("SslAsyncHandshake: failed to allocate handshake job.");
        return -1;
    }

    pJob->m_pIOLink  = pIOLink;
    pJob->m_pSslConn = pSslConn;
    pJob->m_iResult  = 0;
    pJob->m_iErrno   = 0;

    // Suspend I/O events while the handshake is in progress on the worker
    // thread.  The event loop must not read/write on the fd concurrently.
    MultiplexerFactory::getMultiplexer()->suspendRead(pIOLink);
    MultiplexerFactory::getMultiplexer()->suspendWrite(pIOLink);

    int ret = s_pWorkCrew->addJob((ls_lfnodei_t *)pJob);
    if (ret != 0)
    {
        LS_ERROR("SslAsyncHandshake: failed to queue handshake job.");
        // Re-enable events so the connection can fall back to sync handshake
        MultiplexerFactory::getMultiplexer()->continueRead(pIOLink);
        freeJob(pJob);
        return -1;
    }

    LS_DBG_L("SslAsyncHandshake: queued handshake for IOLink %p", pIOLink);
    return 0;
}


void *SslAsyncHandshake::handshakeWorkerFn(ls_lfnodei_t *item)
{
    SslHandshakeJob *pJob = (SslHandshakeJob *)item;
    SslConnection   *pSslConn = pJob->m_pSslConn;

    if (!pSslConn || !pSslConn->getSSL())
    {
        pJob->m_iResult = -1;
        pJob->m_iErrno  = EINVAL;
        return NULL;
    }

    // Clear any prior OpenSSL errors on this worker thread
    ERR_clear_error();

    // Perform the SSL handshake -- this is the CPU-intensive part
    // (RSA/ECDSA key exchange, certificate verification, etc.)
    int ret = SSL_do_handshake(pSslConn->getSSL());

    if (ret == 1)
    {
        pJob->m_iResult = 1;
        pJob->m_iErrno  = 0;
    }
    else
    {
        int err = SSL_get_error(pSslConn->getSSL(), ret);
        switch (err)
        {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            pJob->m_iResult = 0;
            pJob->m_iErrno  = EAGAIN;
            ERR_clear_error();
            break;
        default:
            pJob->m_iResult = -1;
            pJob->m_iErrno  = EIO;
            break;
        }
    }

    return NULL;  // NULL = job processed successfully by WorkCrew convention
}


void SslAsyncHandshake::processCompletedJobs()
{
    if (!s_pFinishedQueue)
        return;

    ls_lfnodei_t *pNode;
    int count = 0;

    while ((pNode = ls_lfqueue_get(s_pFinishedQueue)) != NULL)
    {
        SslHandshakeJob *pJob = (SslHandshakeJob *)pNode;
        NtwkIOLink *pIOLink = pJob->m_pIOLink;
        int result = pJob->m_iResult;

        LS_DBG_L("SslAsyncHandshake: completed job for IOLink %p, result=%d",
                 pIOLink, result);

        freeJob(pJob);

        if (!pIOLink)
            continue;

        // Schedule the resume callback on the main event loop thread.
        // Using schedule_nowait so the event loop wakes up promptly.
        EvtcbQue::getInstance().schedule_nowait(
            asyncHandshakeDoneCb,
            static_cast<evtcbhead_t *>(static_cast<LsiSession *>(pIOLink)),
            (long)result, NULL);

        ++count;
    }

    if (count > 0)
        LS_DBG_L("SslAsyncHandshake: processed %d completed handshakes.",
                 count);
}
