# OpenLiteSpeed Enterprise

A high-performance, enterprise-grade web server based on [OpenLiteSpeed](https://openlitespeed.org/) with critical features backported and newly implemented from LiteSpeed Enterprise.

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Base Version](https://img.shields.io/badge/Base-OLS%201.8.5-green.svg)](https://openlitespeed.org/)

---

## Why This Fork?

Stock OpenLiteSpeed lacks several features that are essential for production shared hosting and high-traffic environments. LiteSpeed Enterprise provides these but requires a paid license. This fork bridges the gap by implementing the most critical enterprise features as open-source additions to OLS 1.8.5.

### Key Differences from Stock OpenLiteSpeed

| Feature | Stock OLS | This Fork |
|---------|-----------|-----------|
| .htaccess support | Rewrite rules only, requires restart | Full Apache-compatible directives, auto-reload |
| ESI (Edge Side Includes) | Not available | Full ESI 1.0 support |
| SSL handshake | Synchronous (blocks event loop) | Async offloading to worker threads |
| WordPress brute force protection | Not available | Built-in with reCAPTCHA |
| VHost loading | All at startup | Just-in-time on first request |
| Dynamic virtual hosting | File-based config only | Redis-backed mass hosting |
| ModSecurity WAF | Synchronous processing | Async thread pool offloading |
| CPU affinity | Basic auto-detect | Configurable per-core pinning |
| VHost bandwidth throttling | Not available | Per-vhost aggregate bandwidth limits |
| Anti-DDoS with reCAPTCHA | Separate systems | Integrated escalation pipeline |
| LSAPI Daemon Mode | Not available | Persistent PHP processes per user |

---

## Installation

### Method 1: Build from Source (Recommended)

#### Prerequisites

```bash
# Ubuntu/Debian 20.04+
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev libpcre3-dev \
    zlib1g-dev libxml2-dev libgeoip-dev libhiredis-dev libmodsecurity-dev \
    libbrotli-dev

# CentOS/RHEL 8+
sudo dnf install -y gcc gcc-c++ cmake git openssl-devel pcre-devel zlib-devel \
    libxml2-devel GeoIP-devel hiredis-devel libmodsecurity-devel brotli-devel

# AlmaLinux / Rocky Linux 9
sudo dnf install -y epel-release
sudo dnf install -y gcc gcc-c++ cmake git openssl-devel pcre-devel zlib-devel \
    libxml2-devel hiredis-devel brotli-devel
```

#### Clone and Build

```bash
git clone https://github.com/kumaraguru1735/openlitespeed-enterprise.git
cd openlitespeed-enterprise

# Create build directory
mkdir build && cd build

# Configure
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DMOD_SECURITY=ON \
    -DCMAKE_INSTALL_PREFIX=/usr/local/lsws

# Build (use all available cores)
make -j$(nproc)

# Install
sudo make install
```

#### Post-Install Setup

```bash
# Create the admin user for WebAdmin console
sudo /usr/local/lsws/admin/misc/admpass.sh

# Create required directories
sudo mkdir -p /usr/local/lsws/logs
sudo mkdir -p /usr/local/lsws/conf/vhosts
sudo mkdir -p /tmp/lshttpd

# Set permissions
sudo chown -R lsadm:lsadm /usr/local/lsws/conf
sudo chmod 750 /usr/local/lsws/conf

# Start the server
sudo /usr/local/lsws/bin/lswsctrl start

# Verify it's running
curl -I http://localhost:8088
```

### Method 2: Replace Existing OpenLiteSpeed

If you already have OLS installed:

```bash
# Stop the existing server
sudo /usr/local/lsws/bin/lswsctrl stop

# Backup current installation
sudo cp -a /usr/local/lsws /usr/local/lsws.backup

# Clone and build
git clone https://github.com/kumaraguru1735/openlitespeed-enterprise.git
cd openlitespeed-enterprise
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DMOD_SECURITY=ON \
    -DCMAKE_INSTALL_PREFIX=/usr/local/lsws
make -j$(nproc)

# Install (preserves existing configs)
sudo make install

# Restart
sudo /usr/local/lsws/bin/lswsctrl start
```

### Build Options

| CMake Option | Default | Description |
|-------------|---------|-------------|
| `MOD_SECURITY` | ON | Build ModSecurity WAF module |
| `MOD_PAGESPEED` | OFF | Build Google PageSpeed module (Linux x64 only) |
| `MOD_LUA` | OFF | Build Lua scripting module (Linux x64) |
| `MOD_REDISVHOST` | AUTO | Build Redis VHost module (auto-detects hiredis) |

### Optional Dependencies

| Library | Package | Required For |
|---------|---------|-------------|
| hiredis | `libhiredis-dev` | Redis Dynamic VHost module |
| libmodsecurity | `libmodsecurity-dev` | ModSecurity WAF module |
| brotli | `libbrotli-dev` | Brotli compression |
| GeoIP | `libgeoip-dev` | GeoIP-based access control |

---

## Usage Guide

### Server Management

```bash
# Start server
sudo /usr/local/lsws/bin/lswsctrl start

# Stop server
sudo /usr/local/lsws/bin/lswsctrl stop

# Graceful restart (zero-downtime)
sudo /usr/local/lsws/bin/lswsctrl restart

# Check status
sudo /usr/local/lsws/bin/lswsctrl status

# Access WebAdmin console (default port 7080)
# https://your-server-ip:7080
```

### Configuration Files

| File | Purpose |
|------|---------|
| `/usr/local/lsws/conf/httpd_config.conf` | Main server configuration |
| `/usr/local/lsws/conf/vhosts/*/vhconf.conf` | Per-vhost configuration |
| `/usr/local/lsws/admin/conf/admin_config.conf` | WebAdmin console config |
| `/usr/local/lsws/logs/error.log` | Server error log |
| `/usr/local/lsws/logs/access.log` | Access log |

### Full Configuration Example

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

serverName                LiteSpeed Enterprise
user                      nobody
group                     nogroup
priority                  0
autoRestart               1
chrootPath                /
enableChroot              0
inMemBufSize              60M
swappingDir               /tmp/lshttpd/swap

# Enterprise: Async SSL for better HTTPS performance
sslAsyncHandshake         1

# Enterprise: Auto CPU core pinning
cpuAffinityMode           1

# Enterprise: Load vhosts on first request (faster startup)
jitVHost                  1

# Enterprise: Anti-DDoS with reCAPTCHA escalation
antiDdosCaptcha           1

# Tuning
maxConnections            10000
maxSSLConnections         10000
connTimeout               300
maxKeepAliveReq           10000
keepAliveTimeout          5
sndBufSize                0
rcvBufSize                0

# Logging
errorlog /usr/local/lsws/logs/error.log {
    logLevel              WARN
    debugLevel            0
    rollingSize           10M
    enableStderrLog       1
}

accesslog /usr/local/lsws/logs/access.log {
    rollingSize           10M
    keepDays              30
    compressArchive       0
}

# Enterprise: WordPress Brute Force Protection
module mod_wpprotect {
    wpProtect             1
    wpProtectMaxRetry     5
    wpProtectLockout      300
    wpProtectAction       1
    recaptchaSiteKey      YOUR_RECAPTCHA_SITE_KEY
    recaptchaSecretKey    YOUR_RECAPTCHA_SECRET_KEY
}

# Enterprise: Async ModSecurity WAF
module mod_security {
    modsecurity           1
    modsecurity_rules_file /etc/modsecurity/owasp-crs/main.conf
    modsecAsync           1
}

# Enterprise: Redis-backed dynamic virtual hosting
module mod_redisvhost {
    redisVHostEnable      1
    redisVHostServer      127.0.0.1:6379
    redisVHostTTL         300
    redisVHostKeyPrefix   vhost:
}

# Listeners
listener HTTP {
    address               *:80
    secure                0
}

listener HTTPS {
    address               *:443
    secure                1
    keyFile               /etc/ssl/private/server.key
    certFile              /etc/ssl/certs/server.crt
}

# Virtual Host example
virtualhost example.com {
    vhRoot                /var/www/example.com
    configFile            conf/vhosts/example.com/vhconf.conf
    allowSymbolLink       1
    enableScript          1
    restrained            1

    # Enterprise: Per-vhost bandwidth limit (10 MB/s)
    vhBandwidthLimit      10485760
}
```

### Per-VHost Configuration

```apacheconf
# /usr/local/lsws/conf/vhosts/example.com/vhconf.conf

docRoot                   $VH_ROOT/public_html
enableGzip                1
enableBr                  1

index {
    useServer             0
    indexFiles            index.php, index.html
}

# LSAPI PHP Handler with Enterprise Daemon Mode
extprocessor lsphp {
    type                  lsapi
    address               uds://tmp/lshttpd/lsphp_example.sock
    maxConns              10
    env                   PHP_LSAPI_CHILDREN=10
    env                   LSAPI_PGRP_MAX_IDLE=300
    initTimeout           60
    retryTimeout          0
    persistConn           1
    respBuffer            0
    autoStart             2
    path                  /usr/local/lsws/lsphp81/bin/lsphp
    backlog               100
    instances             1
    priority              0
    memSoftLimit          2047M
    memHardLimit          2047M
    procSoftLimit         1400
    procHardLimit         1500

    # Enterprise: Daemon mode for persistent PHP processes
    lsapiDaemonMode       1
    lsapiMaxChildren      5
    lsapiMaxIdleTime      300
    lsapiMaxReqs          10000
}

scripthandler {
    add                   lsapi:lsphp php
}

# .htaccess support (auto-detected, no config needed)
# Full Apache-compatible .htaccess is parsed automatically
# Changes are detected every 2 seconds without restart

# Rewrite rules
rewrite {
    enable                1
    autoLoadHtaccess      1
}

# ESI is auto-activated via response header:
#   X-LiteSpeed-Cache-Control: esi=on
# No additional config needed

# Access log
accesslog /var/www/example.com/logs/access.log {
    rollingSize           10M
    keepDays              30
}
```

---

## Enterprise Features

### 1. Full Apache-Compatible .htaccess Support

Comprehensive .htaccess parser (~2,200 lines) with **automatic change detection** (2-second poll interval, no restart needed).

**Supported Directives:**

- **Rewrite & Redirect**: `RewriteEngine`, `RewriteBase`, `RewriteCond`, `RewriteRule`, `Redirect`, `RedirectPermanent`, `RedirectTemp`, `RedirectMatch`, `FallbackResource`
- **Authentication**: `AuthType` (Basic/Digest), `AuthName`, `AuthUserFile`, `AuthGroupFile`, `Require` (valid-user, user, group)
- **Apache 2.4 Access Control**: `Require all granted/denied`, `Require ip`, `Require not ip`, `Require method`
- **Legacy Access Control**: `Order allow,deny`, `Allow from`, `Deny from`, `Satisfy`
- **Environment Variables**: `SetEnv`, `UnsetEnv`, `SetEnvIf`, `SetEnvIfNoCase`
- **MIME Types & Handlers**: `AddType`, `ForceType`, `DefaultType`, `AddHandler`, `SetHandler`
- **Caching & Compression**: `ExpiresActive`, `ExpiresDefault`, `ExpiresByType`, `SetOutputFilter DEFLATE`, `AddOutputFilterByType`
- **Headers**: `Header`, `RequestHeader` with `env=` conditional support
- **Directory & Options**: `DirectoryIndex`, `Options` (Indexes, Includes, ExecCGI, etc.), `ErrorDocument`, `FileETag`, `AddDefaultCharset`
- **Conditional Blocks**: `<IfModule>`, `<IfDefine>`, `<Files>`, `<FilesMatch>`, `<If>`, `<Limit>`, `<LimitExcept>`
- **PHP Configuration**: `php_value`, `php_flag` (blocks `php_admin_value`/`php_admin_flag` for security)

### 2. ESI (Edge Side Includes) Module

Fragment caching for ecommerce and dynamic sites. Activated per-response via `X-LiteSpeed-Cache-Control: esi=on` or `Surrogate-Control: ESI/1.0`.

**Supported Tags:** `<esi:include>`, `<esi:remove>`, `<esi:comment>`, `<esi:try>`/`<esi:attempt>`/`<esi:except>`

### 3. Async SSL Handshake Offloading

Config: `sslAsyncHandshake 1` -- offloads TLS handshakes to worker threads, freeing the event loop.

### 4. WordPress Brute Force Protection

Config: `wpProtect 1` -- monitors `/wp-login.php` and `/xmlrpc.php`, rate-limits per IP with reCAPTCHA challenge.

### 5. Just-In-Time VHost Loading

Config: `jitVHost 1` -- defers vhost config parsing until first request, dramatically reducing startup time.

### 6. Redis Dynamic Virtual Hosting

Config: `redisVHostEnable 1` -- looks up vhost config from Redis for mass hosting without restarts.

### 7. Async ModSecurity WAF

Config: `modsecAsync 1` -- offloads WAF rule evaluation to worker threads.

### 8. CPU Affinity Configuration

Config: `cpuAffinityMode 1` (auto) or `cpuAffinityMode 2` with `cpuAffinityList 0,2,4-7` (manual).

### 9. VHost Bandwidth Throttling (NEW)

Prevents any single virtual host from monopolizing server bandwidth. Critical for shared hosting fairness.

```apacheconf
virtualhost example.com {
    # Limit this vhost to 10 MB/s aggregate bandwidth
    vhBandwidthLimit      10485760
}
```

### 10. Anti-DDoS reCAPTCHA Integration (NEW)

When the server detects connection abuse, instead of immediately banning the client, it redirects to a reCAPTCHA challenge first. Only bans on failure.

```apacheconf
antiDdosCaptcha           1
```

**Escalation flow:** Normal -> Soft Limit -> reCAPTCHA Challenge -> Ban (if challenge fails)

### 11. LSAPI Daemon/ProcessGroup Mode (NEW)

Instead of spawning a new PHP process per request, maintains persistent parent LSPHP processes per user that fork children on demand. Dramatically improves PHP performance for shared hosting.

```apacheconf
extprocessor lsphp {
    type                  lsapi
    lsapiDaemonMode       1
    lsapiMaxChildren      5
    lsapiMaxIdleTime      300
    lsapiMaxReqs          10000
}
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `lsapiDaemonMode` | `0` | 0=off, 1=daemon mode |
| `lsapiMaxChildren` | `5` | Max child processes per daemon |
| `lsapiMaxIdleTime` | `300` | Idle timeout before recycling (seconds) |
| `lsapiMaxReqs` | `10000` | Max requests before recycling child |

---

## Feature Comparison: OLS vs Enterprise vs This Fork

| Feature | OLS | LiteSpeed Enterprise | This Fork |
|---------|-----|---------------------|-----------|
| HTTP/2 & HTTP/3 (QUIC) | Yes | Yes | Yes |
| LSCache (public/private) | Yes | Yes | Yes |
| PageSpeed Module | Yes | Yes | Yes |
| ModSecurity WAF | Yes (sync) | Yes (async) | Yes (async) |
| ESI (Edge Side Includes) | No | Yes | Yes |
| Full .htaccess Support | No (rewrite only) | Yes | Yes |
| .htaccess Auto-Reload | No | Yes | Yes (2s poll) |
| Async SSL Handshake | No | Yes | Yes |
| WP Brute Force Protection | No | Yes | Yes |
| JIT VHost Loading | No | Yes | Yes |
| Redis Dynamic VHost | No | Yes | Yes |
| CPU Affinity Config | Basic | Yes | Yes |
| VHost Bandwidth Throttle | No | Yes | Yes |
| Anti-DDoS + reCAPTCHA | Separate | Integrated | Integrated |
| LSAPI Daemon Mode | No | Yes | Yes |
| Apache Config Import | No | Yes | No |
| LiteMage (Magento) | No | Paid Add-on | Via ESI |
| cPanel/WHM Integration | No | Yes | No |
| Official Support | Community | Paid | Community |
| License | GPLv3 | Proprietary | GPLv3 |

---

## Architecture

```
src/
├── http/
│   ├── htaccessparser.cpp/h      # Full Apache .htaccess parser (~2200 lines)
│   ├── jitconfigloader.cpp/h     # Just-in-time VHost loading
│   ├── clientinfo.h/cpp          # Anti-DDoS with reCAPTCHA escalation
│   ├── httpvhost.h/cpp           # VHost bandwidth throttling
│   ├── vhostmap.cpp/h            # JIT integration
│   └── ...
├── modules/
│   ├── esi/                      # ESI module
│   │   ├── mod_esi.cpp           # Main ESI module
│   │   ├── esiparser.cpp/h       # Streaming ESI tag parser
│   │   └── CMakeLists.txt
│   ├── wpprotect/                # WordPress protection
│   │   ├── mod_wpprotect.cpp     # WP brute force module
│   │   └── CMakeLists.txt
│   ├── redisvhost/               # Redis VHost
│   │   ├── mod_redisvhost.cpp    # Redis dynamic vhost module
│   │   └── CMakeLists.txt
│   └── modsecurity-ls/
│       └── mod_security.cpp      # Async WAF processing
├── extensions/
│   └── lsapi/
│       ├── lsapidaemon.cpp/h     # LSAPI Daemon/ProcessGroup mode
│       └── ...
├── sslpp/
│   ├── sslasynchandshake.cpp/h   # Async SSL handshake offloading
│   └── ...
└── main/
    ├── httpserver.cpp             # JIT VHost, bandwidth throttle, config
    └── lshttpdmain.cpp           # SSL async init, CPU affinity
```

---

## Troubleshooting

### Server won't start
```bash
# Check error log
tail -50 /usr/local/lsws/logs/error.log

# Test config
/usr/local/lsws/bin/lswsctrl configtest

# Check if port is in use
sudo ss -tlnp | grep ':80\|:443'
```

### .htaccess not working
```bash
# Verify autoLoadHtaccess is enabled in vhost config
grep -r "autoLoadHtaccess" /usr/local/lsws/conf/

# Check error log for parse warnings
grep "htaccess" /usr/local/lsws/logs/error.log
```

### Redis VHost not connecting
```bash
# Test Redis connectivity
redis-cli -h 127.0.0.1 -p 6379 ping

# Check for module errors
grep "redisvhost" /usr/local/lsws/logs/error.log

# Verify Redis has data
redis-cli GET "vhost:yourdomain.com"
```

### ModSecurity async issues
```bash
# Check if async is active
grep "modsecAsync" /usr/local/lsws/conf/httpd_config.conf

# Monitor WAF decisions
grep "mod_security" /usr/local/lsws/logs/error.log
```

### PHP performance issues
```bash
# Check LSAPI daemon processes
ps aux | grep lsphp

# Verify daemon mode is enabled
grep "lsapiDaemonMode" /usr/local/lsws/conf/vhosts/*/vhconf.conf
```

---

## systemd Service

Create `/etc/systemd/system/lsws-enterprise.service`:

```ini
[Unit]
Description=LiteSpeed Enterprise Web Server
After=network.target

[Service]
Type=forking
PIDFile=/var/run/lsws-enterprise.pid
ExecStart=/usr/local/lsws/bin/lswsctrl start
ExecStop=/usr/local/lsws/bin/lswsctrl stop
ExecReload=/usr/local/lsws/bin/lswsctrl restart
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable lsws-enterprise
sudo systemctl start lsws-enterprise
```

---

## Credits

- **Base**: [OpenLiteSpeed](https://github.com/litespeedtech/openlitespeed) by LiteSpeed Technologies
- **.htaccess Parser**: [Cloudment/Vectra](https://github.com/Cloudment/Vectra) by Zinidia
- **Enterprise Features**: Implemented with assistance from Claude (Anthropic)

## License

OpenLiteSpeed Enterprise is free software licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).

Copyright (C) 2013-2026 LiteSpeed Technologies, Inc.
Enterprise additions Copyright (C) 2026 kumaraguru1735.
