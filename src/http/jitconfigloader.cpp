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
#include "jitconfigloader.h"

#include <http/httpvhost.h>
#include <http/httpvhostlist.h>
#include <log4cxx/logger.h>
#include <main/configctx.h>
#include <main/httpserver.h>
#include <main/plainconf.h>
#include <util/stringtool.h>
#include <util/xmlnode.h>

#include <string.h>
#include <unistd.h>


JitVHostMap::JitVHostMap()
    : m_iEnabled(0)
{
    pthread_mutex_init(&m_mutex, NULL);
}


JitVHostMap::~JitVHostMap()
{
    clear();
    pthread_mutex_destroy(&m_mutex);
}


int JitVHostMap::addEntry(const char *pName, const char *pConfigPath,
                          const char *pVhRoot, const char *pDomain,
                          const char *pAliases)
{
    if (!pName || !pConfigPath)
        return -1;

    JitVHostEntry *pEntry = new JitVHostEntry();
    if (!pEntry)
        return -1;

    pEntry->setName(pName);
    pEntry->setConfigPath(pConfigPath);

    if (pVhRoot)
        pEntry->setVhRoot(pVhRoot);
    if (pDomain)
        pEntry->setDomain(pDomain);
    else
        pEntry->setDomain(pName);
    if (pAliases)
        pEntry->setAliases(pAliases);

    // Register by vhost name
    m_nameMap.insert(pEntry->getName(), pEntry);

    // Register by primary domain
    const char *pDom = pEntry->getDomain();
    if (pDom && *pDom)
    {
        char achLower[256];
        int len = strlen(pDom);
        if (len >= 256)
            len = 255;
        StringTool::strnlower(pDom, achLower, len);
        achLower[len] = 0;
        m_domainMap.insert(strdup(achLower), pEntry);
    }

    // Register aliases
    if (pAliases && *pAliases)
    {
        const char *p0 = pAliases;
        const char *pEnd = pAliases + strlen(pAliases);
        while (p0 < pEnd)
        {
            while (p0 < pEnd && (*p0 == ',' || *p0 == ' '))
                ++p0;
            const char *p1 = p0;
            while (p1 < pEnd && *p1 != ',' && *p1 != ' ')
                ++p1;
            int aLen = p1 - p0;
            if (aLen > 0 && aLen < 256)
            {
                char achLower[256];
                StringTool::strnlower(p0, achLower, aLen);
                achLower[aLen] = 0;
                m_domainMap.insert(strdup(achLower), pEntry);
            }
            p0 = p1;
        }
    }

    LS_NOTICE("JIT VHost: registered lazy entry [%s] config [%s] domain [%s]",
              pName, pConfigPath, pDom ? pDom : "(none)");
    return 0;
}


JitVHostEntry *JitVHostMap::findByDomain(const char *pDomain) const
{
    if (!pDomain)
        return NULL;
    char achLower[256];
    int len = strlen(pDomain);
    if (len >= 256)
        len = 255;
    StringTool::strnlower(pDomain, achLower, len);
    achLower[len] = 0;

    // Strip www. prefix
    const char *pLookup = achLower;
    DomainMap::const_iterator iter = m_domainMap.find(pLookup);
    if (iter != m_domainMap.end())
        return iter.second();

    if (strncmp(pLookup, "www.", 4) == 0)
    {
        pLookup += 4;
        iter = m_domainMap.find(pLookup);
        if (iter != m_domainMap.end())
            return iter.second();
    }
    return NULL;
}


JitVHostEntry *JitVHostMap::findByName(const char *pName) const
{
    if (!pName)
        return NULL;
    NameMap::const_iterator iter = m_nameMap.find(pName);
    if (iter != m_nameMap.end())
        return iter.second();
    return NULL;
}


HttpVHost *JitVHostMap::loadVHost(const char *pDomain)
{
    JitVHostEntry *pEntry = findByDomain(pDomain);
    if (!pEntry)
        return NULL;

    // Fast path: already loaded
    if (pEntry->isLoaded())
        return pEntry->getVHost();

    // Serialize loading
    pthread_mutex_lock(&m_mutex);

    // Double-check after acquiring lock
    if (pEntry->isLoaded())
    {
        pthread_mutex_unlock(&m_mutex);
        return pEntry->getVHost();
    }

    if (pEntry->isLoading())
    {
        // Another thread is loading this entry right now - wait
        pthread_mutex_unlock(&m_mutex);
        // Spin briefly - this is a rare condition
        for (int i = 0; i < 100; ++i)
        {
            if (pEntry->isLoaded())
                return pEntry->getVHost();
            usleep(10000); // 10ms
        }
        return pEntry->getVHost(); // Return whatever we have
    }

    pEntry->setLoading(true);
    HttpVHost *pVHost = doLoadVHost(pEntry);
    pEntry->setLoading(false);

    pthread_mutex_unlock(&m_mutex);
    return pVHost;
}


HttpVHost *JitVHostMap::doLoadVHost(JitVHostEntry *pEntry)
{
    const char *pConfigPath = pEntry->getConfigPath();
    const char *pName = pEntry->getName();

    LS_NOTICE("JIT VHost: loading config for [%s] from [%s] on first access",
              pName, pConfigPath);

    // Parse the config file
    XmlNode *pConfNode = plainconf::parseFile(pConfigPath, "virtualHostConfig");
    if (!pConfNode)
    {
        LS_ERROR("JIT VHost: failed to parse config file [%s] for vhost [%s]",
                 pConfigPath, pName);
        pEntry->setLoaded(true);  // Mark as attempted to avoid retries
        return NULL;
    }

    // Set up the config context
    ConfigCtx::getCurConfigCtx()->clearVhRoot();
    ConfigCtx::getCurConfigCtx()->setVhName(pName);

    const char *pVhRoot = pEntry->getVhRoot();
    const char *pDomain = pEntry->getDomain();
    const char *pAliases = pEntry->getAliases();

    // If vhRoot not stored, try to get it from the config
    if (!pVhRoot || !*pVhRoot)
        pVhRoot = pConfNode->getChildValue("vhRoot");

    if (!pDomain || !*pDomain)
        pDomain = pConfNode->getChildValue("vhDomain");

    if (!pAliases || !*pAliases)
        pAliases = pConfNode->getChildValue("vhAliases");

    // Use the static configVHost method to create the vhost
    HttpVHost *pVHost = HttpVHost::configVHost(pConfNode, pName,
                        pDomain, pAliases, pVhRoot, pConfNode);

    if (pVHost)
    {
        // Add to the server's vhost map
        if (HttpServer::getInstance().addVHost(pVHost) != 0)
        {
            LS_ERROR("JIT VHost: failed to add vhost [%s] to server", pName);
            delete pVHost;
            pVHost = NULL;
        }
        else
        {
            // Map to listeners
            if (pDomain)
                HttpServer::getInstance().mapListenerToVHost("*", pVHost, pDomain);
            if (pAliases)
                HttpServer::getInstance().mapListenerToVHost("*", pVHost, pAliases);

            pEntry->setVHost(pVHost);
            pEntry->setLoadTime(time(NULL));
            LS_NOTICE("JIT VHost: successfully loaded vhost [%s]", pName);
        }
    }
    else
    {
        LS_ERROR("JIT VHost: failed to configure vhost [%s]", pName);
    }

    pEntry->setLoaded(true);
    delete pConfNode;
    return pVHost;
}


int JitVHostMap::size() const
{
    return m_nameMap.size();
}


void JitVHostMap::clear()
{
    // Free domain map keys (strdup'd)
    DomainMap::iterator dIter;
    for (dIter = m_domainMap.begin(); dIter != m_domainMap.end();
         dIter = m_domainMap.next(dIter))
    {
        // Keys allocated with strdup
        free((void *)dIter.first());
    }
    m_domainMap.clear();

    // Delete entries via name map (entries are shared, so only delete once)
    NameMap::iterator nIter;
    for (nIter = m_nameMap.begin(); nIter != m_nameMap.end();
         nIter = m_nameMap.next(nIter))
    {
        delete nIter.second();
    }
    m_nameMap.clear();
}
