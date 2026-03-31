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

---

## Enterprise Features

### 1. Full Apache-Compatible .htaccess Support

The most significant limitation of stock OLS for shared hosting is its minimal .htaccess support. This fork includes a comprehensive .htaccess parser (~2,200 lines) that supports the vast majority of Apache directives users rely on.

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

**Credits:** .htaccess parser originally developed by [Cloudment/Vectra](https://github.com/Cloudment/Vectra).

---

### 2. ESI (Edge Side Includes) Module

ESI enables partial page caching by allowing you to cache the majority of a page while punching holes for dynamic fragments. This is essential for ecommerce platforms like Magento (LiteMage) and any site with user-specific content embedded in otherwise cacheable pages.

**Supported Tags:**
- `<esi:include src="/fragment.html" alt="/fallback.html" onerror="continue" />`
- `<esi:remove>...</esi:remove>`
- `<esi:comment text="..." />`
- `<esi:try><esi:attempt>...</esi:attempt><esi:except>...</esi:except></esi:try>`

**Activation:**

ESI processing is triggered per-response when the backend sends either:
```
X-LiteSpeed-Cache-Control: esi=on
```
or:
```
Surrogate-Control: content="ESI/1.0"
```

**How It Works:**
1. The module hooks into the response processing pipeline
2. When the activation header is detected, it buffers the response body
3. Parses all ESI tags using a streaming parser
4. Fetches `<esi:include>` fragments via internal loopback HTTP requests
5. Assembles the final response with fragments inlined
6. Supports `alt` URLs as fallbacks and `onerror="continue"` for graceful degradation

**Limits:**
- Max response body for ESI processing: 8 MB
- Max fragment size: 2 MB
- Max recursion depth: 5 levels
- Fragment fetch timeout: 5 seconds

**Configuration:** The module is prelinked and activates automatically when the response header is present. No additional configuration required.

---

### 3. Async SSL Handshake Offloading

TLS handshakes involve CPU-intensive cryptographic operations (RSA/ECDSA key exchange, certificate verification). In stock OLS, these block the main event loop, reducing throughput under heavy HTTPS load.

This feature offloads SSL handshakes to a dedicated worker thread pool, freeing the event loop to continue processing other connections.

**Configuration:**
```
sslAsyncHandshake    1
```

**How It Works:**
1. When a new SSL connection arrives, the handshake is queued to the thread pool
2. The event loop suspends I/O on that connection and continues processing others
3. A worker thread performs `SSL_do_handshake()` (the expensive crypto operation)
4. On completion, the event loop is notified via `EvtcbQue` and the connection resumes
5. If queueing fails, falls back to synchronous handshake transparently

**Thread Pool:** 2-16 worker threads (default: 4), configurable.

---

### 4. WordPress Brute Force Protection

Built-in protection against brute force attacks on WordPress login endpoints, without requiring any WordPress plugins.

**Protected Endpoints:**
- `POST /wp-login.php`
- `POST /xmlrpc.php`

**Features:**
- Per-IP attempt tracking using shared memory (works across all worker processes)
- Configurable threshold and lockout duration
- Two blocking modes: flat deny (403) or reCAPTCHA challenge
- Automatic whitelisting of logged-in admins (via `wordpress_logged_in_*` cookie)
- Periodic cleanup of expired records (every 60 seconds)
- Capacity: 4,096 concurrent tracked IPs with LRU eviction

**Configuration:**
```
module mod_wpprotect {
    wpProtect              1
    wpProtectMaxRetry      5
    wpProtectLockout       300
    wpProtectAction        1
    recaptchaSiteKey       YOUR_SITE_KEY
    recaptchaSecretKey     YOUR_SECRET_KEY
}
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `wpProtect` | `0` | Enable/disable (0 or 1) |
| `wpProtectMaxRetry` | `5` | Max POST attempts before lockout |
| `wpProtectLockout` | `300` | Lockout duration in seconds |
| `wpProtectAction` | `1` | 0 = flat 403 deny, 1 = reCAPTCHA challenge |
| `recaptchaSiteKey` | `""` | Google reCAPTCHA v2 site key |
| `recaptchaSecretKey` | `""` | Google reCAPTCHA v2 secret key |

**reCAPTCHA Flow:**
1. User exceeds attempt threshold
2. Server responds with a styled 403 page containing a reCAPTCHA widget
3. User completes the challenge, which sets a verification cookie
4. Subsequent requests with the cookie are allowed and the IP's counter is reset

---

### 5. Just-In-Time VHost Configuration Loading

For servers with hundreds or thousands of virtual hosts, loading all vhost configs at startup significantly increases restart time and memory usage. JIT loading defers config parsing until the first request for each domain.

**Configuration:**
```
jitVHost    1
```

**How It Works:**
1. At startup, the server registers lightweight domain-to-config-path mappings instead of parsing full vhost configs
2. When a request arrives for a domain whose vhost hasn't been loaded, `JitVHostMap::loadVHost()` is triggered
3. The config file is parsed, the `HttpVHost` is created, and it's added to the server's vhost map
4. Subsequent requests for the same domain use the cached vhost (no re-parsing)

**Thread Safety:** Uses mutex with double-checked locking to prevent concurrent loads of the same vhost.

**Startup Impact:** For a server with 500 vhosts where only 50 are active, JIT loading means the server starts in a fraction of the time and only uses memory for the 50 active vhosts.

---

### 6. Redis-Backed Dynamic Virtual Hosting

For mass hosting environments, managing thousands of vhost config files becomes impractical. This module looks up vhost configuration from Redis, enabling dynamic provisioning without server restarts.

**Configuration:**
```
module mod_redisvhost {
    redisVHostEnable       1
    redisVHostServer       127.0.0.1:6379
    redisVHostPassword     your_password
    redisVHostTTL          300
    redisVHostKeyPrefix    vhost:
}
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `redisVHostEnable` | `0` | Enable/disable |
| `redisVHostServer` | `127.0.0.1:6379` | Redis server host:port |
| `redisVHostPassword` | `""` | Redis AUTH password |
| `redisVHostTTL` | `300` | In-memory cache TTL (seconds) |
| `redisVHostKeyPrefix` | `vhost:` | Redis key prefix |

**Redis Key Format:**
```
SET vhost:example.com '{
    "docRoot":    "/var/www/example.com/public_html",
    "phpVersion": "lsphp81",
    "accessLog":  "/var/log/ols/example.com-access.log",
    "errorLog":   "/var/log/ols/example.com-error.log",
    "aliases":    "www.example.com",
    "uid":        1001,
    "gid":        1001,
    "sslCert":    "/etc/ssl/certs/example.com.crt",
    "sslKey":     "/etc/ssl/private/example.com.key"
}'
```

**Features:**
- In-memory cache (256-bucket hash table) to minimize Redis queries
- Automatic reconnection with configurable backoff (5-second cooldown)
- `www.` prefix stripping for domain normalization
- Sets environment variables (`REDIS_VHOST_DOCROOT`, `REDIS_VHOST_PHPVER`, etc.) for use by rewrite rules
- Validates docRoot directory exists before applying
- Falls through to file-based config if Redis lookup fails

**Dependency:** Requires `hiredis` library. The build system gracefully skips the module if hiredis is not installed.

---

### 7. Async ModSecurity WAF Processing

Stock OLS runs ModSecurity rule evaluation synchronously, blocking the request while rules are checked. For complex rulesets (OWASP CRS with 100+ rules), this adds measurable latency.

This enhancement offloads ModSecurity evaluation to a worker thread pool, allowing the event loop to continue processing other requests while WAF rules are evaluated.

**Configuration:**
```
module mod_security {
    modsecurity                 1
    modsecurity_rules_file      /etc/modsecurity/main.conf
    modsecAsync                 1
}
```

**How It Works:**
1. At the URI_MAP hook, if async is enabled, the module snapshots all request metadata (headers, method, URI, client IP, etc.)
2. The snapshot is queued to a 4-thread WorkCrew pool
3. The HTTP session returns `LSI_SUSPEND`, freeing the event loop
4. The worker thread creates a ModSecurity transaction and runs phases 1-3
5. Results (allow/deny/redirect) are stored in the session's `ModData`
6. The worker signals the event loop via `g_api->create_session_resume_event()`
7. On resume, the URI_MAP hook checks the result and either allows or denies the request

**Thread Safety:** Uses `std::atomic<int>` with explicit memory ordering for the handoff between worker and event loop threads.

**Fallback:** If async queueing fails, the module transparently falls back to synchronous processing.

---

### 8. CPU Affinity Configuration

Pin worker processes to specific CPU cores for better L1/L2 cache utilization and reduced context switching overhead.

**Configuration:**
```
cpuAffinityMode    2
cpuAffinityList    0,2,4-7
```

| Parameter | Values | Description |
|-----------|--------|-------------|
| `cpuAffinityMode` | `0` | Disabled |
| | `1` | Auto (assigns cores round-robin) |
| | `2` | Manual (uses `cpuAffinityList`) |
| `cpuAffinityList` | `"0,2,4-7"` | Comma-separated CPU IDs and ranges |

**Backward Compatible:** When `cpuAffinityMode` is not set but `cpuAffinity` (legacy OLS option) is > 0, auto mode is used.

---

## Building from Source

### Prerequisites

```bash
# Ubuntu/Debian
apt-get install build-essential cmake libssl-dev libpcre3-dev zlib1g-dev \
    libxml2-dev libgeoip-dev libhiredis-dev libmodsecurity-dev

# CentOS/RHEL
yum install gcc gcc-c++ cmake openssl-devel pcre-devel zlib-devel \
    libxml2-devel GeoIP-devel hiredis-devel libmodsecurity-devel
```

**Optional dependencies:**
- `hiredis` - Required for Redis VHost module (skipped if not found)
- `libmodsecurity` - Required for ModSecurity module
- `brotli` - For Brotli compression support

### Build

```bash
git clone https://github.com/kumaraguru1735/openlitespeed-enterprise.git
cd openlitespeed-enterprise

mkdir build && cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DMOD_SECURITY=ON \
    -DCMAKE_INSTALL_PREFIX=/usr/local/lsws
make -j$(nproc)
sudo make install
```

### Build Options

| CMake Option | Default | Description |
|-------------|---------|-------------|
| `MOD_SECURITY` | ON | Build ModSecurity module |
| `MOD_PAGESPEED` | OFF | Build PageSpeed module (Linux x64 only) |
| `MOD_LUA` | OFF | Build Lua module (Linux x64) |
| `MOD_REDISVHOST` | AUTO | Build Redis VHost module (auto-detects hiredis) |

---

## Quick Configuration Example

A minimal config showcasing enterprise features:

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

serverName                LiteSpeed Enterprise
sslAsyncHandshake         1
cpuAffinityMode           1
jitVHost                  1

module mod_wpprotect {
    wpProtect             1
    wpProtectMaxRetry     5
    wpProtectLockout      300
    wpProtectAction       1
    recaptchaSiteKey      6Le...YOUR_KEY
    recaptchaSecretKey    6Le...YOUR_SECRET
}

module mod_security {
    modsecurity           1
    modsecurity_rules_file /etc/modsecurity/owasp-crs/main.conf
    modsecAsync           1
}

module mod_redisvhost {
    redisVHostEnable      1
    redisVHostServer      127.0.0.1:6379
    redisVHostTTL         300
}

listener Default {
    address               *:443
    secure                1
    ...
}
```

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
| .htaccess Auto-Reload | No | Yes | Partial |
| Async SSL Handshake | No | Yes | Yes |
| WP Brute Force Protection | No | Yes | Yes |
| JIT VHost Loading | No | Yes | Yes |
| Redis Dynamic VHost | No | Yes | Yes |
| CPU Affinity Config | Basic | Yes | Yes |
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
│   ├── htaccessparser.cpp/h      # Full Apache .htaccess parser
│   ├── jitconfigloader.cpp/h     # Just-in-time VHost loading
│   ├── vhostmap.cpp/h            # Modified for JIT integration
│   └── ...
├── modules/
│   ├── esi/                      # ESI module
│   │   ├── mod_esi.cpp           # Main ESI module (970 lines)
│   │   ├── esiparser.cpp/h       # Streaming ESI tag parser
│   │   └── CMakeLists.txt
│   ├── wpprotect/                # WordPress protection
│   │   ├── mod_wpprotect.cpp     # WP brute force module (822 lines)
│   │   └── CMakeLists.txt
│   ├── redisvhost/               # Redis VHost
│   │   ├── mod_redisvhost.cpp    # Redis dynamic vhost module (935 lines)
│   │   └── CMakeLists.txt
│   └── modsecurity-ls/
│       └── mod_security.cpp      # Enhanced with async WAF processing
├── sslpp/
│   ├── sslasynchandshake.cpp/h   # Async SSL handshake offloading
│   └── ...
└── main/
    ├── httpserver.cpp             # JIT VHost & config integration
    └── lshttpdmain.cpp           # SSL async init & CPU affinity
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
