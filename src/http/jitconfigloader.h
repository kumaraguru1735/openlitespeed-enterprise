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
#ifndef JITCONFIGLOADER_H
#define JITCONFIGLOADER_H

#include <lsdef.h>
#include <util/autostr.h>
#include <util/hashstringmap.h>

#include <pthread.h>
#include <time.h>

class HttpVHost;
class XmlNode;


/**
 * JitVHostEntry stores the config file path and domain mappings for a
 * virtual host that has not yet been fully loaded.  When jitVHost is
 * enabled the server registers these lightweight entries at startup
 * instead of parsing the full vhost config.
 */
class JitVHostEntry
{
public:
    JitVHostEntry()
        : m_pVHost(NULL)
        , m_bLoaded(false)
        , m_bLoading(false)
        , m_loadTime(0)
    {}

    ~JitVHostEntry() {}

    void setName(const char *p)             { m_sName.setStr(p); }
    const char *getName() const             { return m_sName.c_str(); }

    void setConfigPath(const char *p)       { m_sConfigPath.setStr(p); }
    const char *getConfigPath() const       { return m_sConfigPath.c_str(); }

    void setVhRoot(const char *p)           { m_sVhRoot.setStr(p); }
    const char *getVhRoot() const           { return m_sVhRoot.c_str(); }

    void setDomain(const char *p)           { m_sDomain.setStr(p); }
    const char *getDomain() const           { return m_sDomain.c_str(); }

    void setAliases(const char *p)          { m_sAliases.setStr(p); }
    const char *getAliases() const          { return m_sAliases.c_str(); }

    HttpVHost *getVHost() const             { return m_pVHost; }
    void setVHost(HttpVHost *p)             { m_pVHost = p; }

    bool isLoaded() const                   { return m_bLoaded; }
    void setLoaded(bool v)                  { m_bLoaded = v; }

    bool isLoading() const                  { return m_bLoading; }
    void setLoading(bool v)                 { m_bLoading = v; }

    time_t getLoadTime() const              { return m_loadTime; }
    void setLoadTime(time_t t)              { m_loadTime = t; }

private:
    AutoStr2    m_sName;
    AutoStr2    m_sConfigPath;
    AutoStr2    m_sVhRoot;
    AutoStr2    m_sDomain;
    AutoStr2    m_sAliases;
    HttpVHost  *m_pVHost;
    // volatile: read outside mutex in double-checked locking pattern
    volatile bool   m_bLoaded;
    volatile bool   m_bLoading;
    time_t      m_loadTime;

    LS_NO_COPY_ASSIGN(JitVHostEntry);
};


/**
 * JitVHostMap maintains a mapping from domain name to JitVHostEntry.
 * When a request arrives for a domain whose vhost has not been loaded,
 * the JIT loader parses the config file and creates the HttpVHost on
 * the fly.
 *
 * Thread safety: a mutex protects the load operation so that concurrent
 * requests for the same domain do not cause double-loading.
 */
class JitVHostMap
{
public:
    JitVHostMap();
    ~JitVHostMap();

    void setEnabled(int v)              { m_iEnabled = v; }
    int  isEnabled() const              { return m_iEnabled; }

    /**
     * Register a vhost config path for lazy loading.  Called during
     * startup when jitVHost is enabled.
     */
    int addEntry(const char *pName, const char *pConfigPath,
                 const char *pVhRoot, const char *pDomain,
                 const char *pAliases);

    /**
     * Look up an entry by domain name.
     */
    JitVHostEntry *findByDomain(const char *pDomain) const;

    /**
     * Look up an entry by vhost name.
     */
    JitVHostEntry *findByName(const char *pName) const;

    /**
     * Attempt to load the vhost for the given domain on demand.
     * Returns the fully configured HttpVHost, or NULL on failure.
     * The result is cached so subsequent calls return immediately.
     */
    HttpVHost *loadVHost(const char *pDomain);

    /**
     * Return the number of registered (but possibly not yet loaded)
     * vhost entries.
     */
    int size() const;

    /**
     * Clear all entries.
     */
    void clear();

private:
    HttpVHost *doLoadVHost(JitVHostEntry *pEntry);

    typedef HashStringMap<JitVHostEntry *> DomainMap;
    typedef HashStringMap<JitVHostEntry *> NameMap;

    DomainMap       m_domainMap;     // domain -> entry
    NameMap         m_nameMap;       // vhost name -> entry
    int             m_iEnabled;
    pthread_mutex_t m_mutex;

    LS_NO_COPY_ASSIGN(JitVHostMap);
};


#endif // JITCONFIGLOADER_H
