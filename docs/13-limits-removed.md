# Limits Removed - Enterprise Unlimited Mode

## Overview

Stock OpenLiteSpeed ships with conservative limits designed for small
single-site deployments. These defaults work fine for a personal blog but
cause problems on production servers handling thousands of concurrent
connections across hundreds of virtual hosts.

This fork raises or removes many of these limits to match LiteSpeed Enterprise
behavior. Below is a complete breakdown of every limit that was changed.

## Complete Table of All Raised Limits

### Connection Limits

| Limit                    | Stock OLS       | This Fork       | LiteSpeed Enterprise |
|--------------------------|-----------------|-----------------|----------------------|
| Per-client hard limit    | 100             | 10,000          | 10,000               |
| Per-client soft limit    | 10,000          | INT_MAX (none)  | Configurable         |
| Max connections          | 2,000           | 10,000          | Unlimited            |
| Max SSL connections      | 2,000           | 10,000          | Unlimited            |
| Keep-alive max requests  | 100             | 10,000          | Configurable         |
| Max FD index             | 100,000         | 100,000         | Unlimited            |

### Worker Limits

| Limit                    | Stock OLS       | This Fork       | LiteSpeed Enterprise |
|--------------------------|-----------------|-----------------|----------------------|
| Max workers (processes)  | Auto (CPU cores)| Configurable    | Configurable         |
| Max ext processor conns  | 1-10,000        | 1-10,000        | Unlimited            |
| Backlog queue size       | 100             | 100 (tunable)   | Configurable         |

### Cache Limits

| Limit                    | Stock OLS       | This Fork       | LiteSpeed Enterprise |
|--------------------------|-----------------|-----------------|----------------------|
| Cache module max size    | 10 MB default   | 10 MB (tunable) | Unlimited            |
| In-memory buffer size    | 60 MB           | 60 MB (tunable) | Configurable         |

### PHP / LSAPI Limits

| Limit                    | Stock OLS       | This Fork       | LiteSpeed Enterprise |
|--------------------------|-----------------|-----------------|----------------------|
| LSAPI max requests       | 10,000          | 10,000 (tunable)| Configurable         |
| Max children per daemon  | N/A (no daemon) | 5-100           | Configurable         |
| Daemon idle timeout      | N/A (no daemon) | 10-86400 sec    | Configurable         |
| Process soft limit       | 1,400           | 1,400 (tunable) | Configurable         |
| Process hard limit       | 1,500           | 1,500 (tunable) | Configurable         |
| Memory soft limit        | 2047 MB         | 2047 MB (tunable)| Configurable        |
| Memory hard limit        | 2047 MB         | 2047 MB (tunable)| Configurable        |

### Per-Client Limits

| Limit                    | Stock OLS       | This Fork       | LiteSpeed Enterprise |
|--------------------------|-----------------|-----------------|----------------------|
| Hard connection limit    | 100             | 10,000          | 10,000               |
| Soft connection limit    | 10,000          | No limit        | Configurable         |
| Grace period             | 15 sec          | 15 sec          | Configurable         |
| Ban period               | 60 sec          | 300 sec         | Configurable         |
| Anti-DDoS reCAPTCHA      | Not available   | Available       | Available            |

### Bandwidth / Throttling Limits

| Limit                    | Stock OLS       | This Fork       | LiteSpeed Enterprise |
|--------------------------|-----------------|-----------------|----------------------|
| Per-vhost bandwidth      | Not available   | Configurable    | Configurable         |
| Per-client throttle      | Basic           | Configurable    | Configurable         |

## How to Verify Limits Are Removed

### Check Compiled Defaults

You can verify the compiled-in defaults by examining the source or checking
the running configuration:

```bash
# Check the per-client hard limit in the binary
grep -r "s_iHardLimitPC" /root/OLS/src/http/clientinfo.cpp
# Should show: int ClientInfo::s_iHardLimitPC = 10000;

# Check the per-client soft limit
grep -r "s_iSoftLimitPC" /root/OLS/src/http/clientinfo.cpp
# Should show: int ClientInfo::s_iSoftLimitPC = INT_MAX;

# Check ban period default
grep -r "s_iBanPeriod" /root/OLS/src/http/clientinfo.cpp
# Should show: int ClientInfo::s_iBanPeriod = 300;
```

### Check Running Configuration

```bash
# View the active config
cat /usr/local/lsws/conf/httpd_config.conf | grep -i "max\|limit\|conn"

# Test config validity
/usr/local/lsws/bin/lswsctrl configtest

# Check current connection count vs limits
ss -s
```

### Check System-Level Limits

```bash
# Check file descriptor limits for the server process
cat /proc/$(pgrep lshttpd | head -1)/limits 2>/dev/null

# Check open file descriptors
ls /proc/$(pgrep lshttpd | head -1)/fd 2>/dev/null | wc -l

# Check system-wide file descriptor limit
cat /proc/sys/fs/file-max
```

## How to Further Customize Limits

All limits can be overridden in your config file. The raised defaults are
starting points, not fixed values.

### Connection Limits

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

# Server-wide connection limits
maxConnections            20000
maxSSLConnections         20000
connTimeout               300
maxKeepAliveReq           50000
keepAliveTimeout          5

# Per-client connection limits
perClientConnLimit {
    softLimit             1000
    hardLimit             5000
    gracePeriod           15
    banPeriod             600
}
```

### PHP / LSAPI Limits

```apacheconf
# In vhconf.conf extprocessor block
extprocessor lsphp {
    type                  lsapi
    maxConns              20
    memSoftLimit          4096M
    memHardLimit          4096M
    procSoftLimit         2000
    procHardLimit         2500
    lsapiMaxChildren      10
    lsapiMaxReqs          50000
}
```

## Comparison Table: Stock OLS vs LiteSpeed Enterprise vs This Fork

| Capability                     | Stock OLS          | LiteSpeed Enterprise | This Fork          |
|--------------------------------|--------------------|----------------------|--------------------|
| **Licensing**                  | GPLv3 (free)       | Proprietary (paid)   | GPLv3 (free)       |
| **Per-client hard limit**      | 100                | 10,000               | 10,000             |
| **Anti-DDoS reCAPTCHA**        | No                 | Yes                  | Yes                |
| **JIT VHost loading**          | No                 | Yes                  | Yes                |
| **LSAPI daemon mode**          | No                 | Yes                  | Yes                |
| **Async ModSecurity**          | No                 | Yes                  | Yes                |
| **Async SSL handshake**        | No                 | Yes                  | Yes                |
| **ESI support**                | No                 | Yes                  | Yes                |
| **Full .htaccess**             | Rewrite only       | Yes                  | Yes                |
| **VHost bandwidth throttle**   | No                 | Yes                  | Yes                |
| **Redis dynamic vhost**        | No                 | Yes                  | Yes                |
| **CPU affinity config**        | Basic              | Yes                  | Yes                |
| **WordPress brute force**      | No                 | Yes                  | Yes                |
| **cPanel integration**         | No                 | Yes                  | No                 |
| **Official support**           | Community          | Paid                 | Community          |

## What "Unlimited" Actually Means

When we say limits are "removed," we do not mean the server can handle
infinite connections. Every server is bounded by physical resources:

- **File descriptors**: Each connection uses one FD. The OS limits total FDs.
- **Memory**: Each connection consumes RAM for buffers, SSL state, and
  request parsing.
- **CPU**: Each active request needs CPU time for processing.
- **Network bandwidth**: The NIC and upstream links have finite capacity.

"Unlimited" in this context means the server software does not impose an
artificial cap below what the hardware can handle. The actual limit is
determined by your hardware and OS configuration.

### Rule of Thumb for Capacity

| Resource        | Per Connection    | 10,000 Connections |
|-----------------|-------------------|--------------------|
| File descriptors| 1 FD              | 10,000 FDs         |
| Memory (HTTP)   | ~8-16 KB          | 80-160 MB          |
| Memory (HTTPS)  | ~30-50 KB         | 300-500 MB         |
| Memory (WebSocket)| ~4-8 KB         | 40-80 MB           |

## Tuning System-Level Limits

The server can only use what the OS allows. Here is how to raise OS-level
limits to match the server's capabilities.

### File Descriptors (ulimit)

```bash
# Check current limit
ulimit -n

# Set permanently in /etc/security/limits.conf
# Add: *  soft  nofile  65535
# Add: *  hard  nofile  65535
```

### Kernel Network Tuning (sysctl)

Create `/etc/sysctl.d/99-ols-tuning.conf` with these key settings:

```
fs.file-max = 2097152
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 15
net.ipv4.ip_local_port_range = 1024 65535
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
```

Apply with `sudo sysctl --system`.

### systemd Service Limits

If running OLS under systemd, create a drop-in override:

```bash
# /etc/systemd/system/lsws.service.d/limits.conf
[Service]
LimitNOFILE=65535
LimitNPROC=65535
```

Then run `sudo systemctl daemon-reload && sudo systemctl restart lsws`.

## Complete Production Tuning Config for High-Traffic Server

This configuration is designed for a server handling 10,000+ concurrent
connections across hundreds of virtual hosts:

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

serverName                High-Traffic Production
user                      nobody
group                     nogroup
autoRestart               1

jitVHost                  1
sslAsyncHandshake         1
cpuAffinityMode           1
antiDdosCaptcha           1

maxConnections            20000
maxSSLConnections         20000
connTimeout               300
maxKeepAliveReq           50000
keepAliveTimeout          5
inMemBufSize              120M

perClientConnLimit {
    softLimit             1000
    hardLimit             5000
    gracePeriod           15
    banPeriod             600
}

module mod_security {
    modsecurity               1
    modsecurity_rules_file    /etc/modsecurity/main.conf
    modsecAsync               1
}
```

Pair this with the sysctl and ulimit tuning shown above. Monitor with
`ss -s` for connections and `tail -f /usr/local/lsws/logs/error.log` for
limit warnings.
