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
#ifndef LSAPICONFIG_H
#define LSAPICONFIG_H



#include <extensions/localworkerconfig.h>

class LsapiConfig : public LocalWorkerConfig
{
    int     m_iDaemonMode;      // 0=off, 1=daemon, 2=processgroup
    int     m_iDaemonMaxChildren;
    int     m_iDaemonMaxIdleTime;
    int     m_iDaemonMaxReqs;

public:
    explicit LsapiConfig(const char *pName);
    LsapiConfig();

    ~LsapiConfig();

    // Daemon/ProcessGroup mode accessors
    int getDaemonMode() const           {   return m_iDaemonMode;           }
    void setDaemonMode(int mode)        {   m_iDaemonMode = mode;           }

    int getDaemonMaxChildren() const    {   return m_iDaemonMaxChildren;    }
    void setDaemonMaxChildren(int n)    {   m_iDaemonMaxChildren = n;       }

    int getDaemonMaxIdleTime() const    {   return m_iDaemonMaxIdleTime;    }
    void setDaemonMaxIdleTime(int s)    {   m_iDaemonMaxIdleTime = s;       }

    int getDaemonMaxReqs() const        {   return m_iDaemonMaxReqs;        }
    void setDaemonMaxReqs(int n)        {   m_iDaemonMaxReqs = n;           }

    int isDaemonEnabled() const
    {   return m_iDaemonMode != 0;  }
};

#endif
