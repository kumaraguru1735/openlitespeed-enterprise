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
#include "lsapiworker.h"
#include "lsapiconfig.h"
#include "lsapiconn.h"
#include "lsapidaemon.h"
#include <http/handlertype.h>
#include <http/serverprocessconfig.h>
#include <log4cxx/logger.h>
#include <main/configctx.h>
#include <main/mainserverconfig.h>

#include <pwd.h>
#include <stdio.h>


LsapiWorker::LsapiWorker(const char *pName)
    : LocalWorker(HandlerType::HT_LSAPI)
    , m_pDaemon(NULL)
{
    setConfigPointer(new LsapiConfig(pName));
}


LsapiWorker::~LsapiWorker()
{
    // Daemon cleanup is handled by LsapiDaemon::shutdownAll()
    // or via LsapiDaemon::removeDaemon(). Don't delete m_pDaemon
    // here since it's managed by the static registry.
    m_pDaemon = NULL;
}

ExtConn *LsapiWorker::newConn()
{
    return new LsapiConn();
}


int LsapiWorker::startDaemonWorker()
{
    LsapiConfig &config = getConfig();

    // Resolve the username from uid
    uid_t uid = config.getUid();
    gid_t gid = config.getGid();

    if ((int)uid == -1)
        uid = ServerProcessConfig::getInstance().getUid();
    if ((int)gid == -1)
        gid = ServerProcessConfig::getInstance().getGid();

    // Get username for the socket path
    const char *pUsername = NULL;
    char achUser[256];
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name)
        pUsername = pw->pw_name;
    else
    {
        snprintf(achUser, sizeof(achUser), "uid%d", uid);
        pUsername = achUser;
    }

    // Get or create the daemon for this user
    m_pDaemon = LsapiDaemon::getDaemon(pUsername, uid, gid);
    if (!m_pDaemon)
    {
        LS_ERROR("[LsapiWorker] Failed to get daemon for user [%s]",
                 pUsername);
        return -1;
    }

    // Copy config settings to the daemon
    LsapiDaemonConfig &dconf = m_pDaemon->getConfig();
    dconf.m_iDaemonMode = config.getDaemonMode();
    dconf.m_iMaxChildren = config.getDaemonMaxChildren();
    dconf.m_iMaxIdleTime = config.getDaemonMaxIdleTime();
    dconf.m_iMaxReqs = config.getDaemonMaxReqs();

    // Update the worker's URL to point to the daemon socket
    // so that connections go to the daemon's Unix socket
    char achUrl[512];
    snprintf(achUrl, sizeof(achUrl), "uds:/%s",
             m_pDaemon->getSocketPath());

    // Start the daemon if not already running
    int pid = m_pDaemon->start(&config, this);
    if (pid <= 0)
    {
        LS_ERROR("[LsapiWorker] Failed to start daemon for user [%s]",
                 pUsername);
        return -1;
    }

    // Update the server address to the daemon socket so the web server
    // connects to the daemon's socket for LSAPI requests
    config.setURL(achUrl);
    config.updateServerAddr(achUrl);

    LS_NOTICE("[LsapiWorker] Daemon mode active for [%s], "
              "socket [%s], daemon pid %d",
              getName(), m_pDaemon->getSocketPath(), pid);

    return 0;
}


int LsapiWorker::startEx()
{
    LsapiConfig &config = getConfig();

    // If daemon mode is enabled, use the daemon path
    if (config.isDaemonEnabled() && config.getSelfManaged()
        && config.getURL() && config.getCommand())
    {
        LS_NOTICE("[LsapiWorker] Starting [%s] in daemon mode %d",
                  getName(), config.getDaemonMode());
        return startDaemonWorker();
    }

    // Fall back to normal worker start
    int ret = 1;
    if (config.getSelfManaged() && (config.getURL()) && (config.getCommand()))
        ret = startWorker();
    return ret;
}


int LsapiWorker::stop()
{
    if (m_pDaemon)
    {
        m_pDaemon->stop();
        m_pDaemon = NULL;
    }
    return LocalWorker::stop();
}


void LsapiWorker::onTimer()
{
    if (m_pDaemon)
        m_pDaemon->onTimer();
    LocalWorker::onTimer();
}
