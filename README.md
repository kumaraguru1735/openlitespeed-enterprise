# OpenLiteSpeed Enterprise - Unlimited Open Source Web Server

> **OpenLiteSpeed** with enterprise features, no limits, no license fees. Drop-in replacement for LiteSpeed Enterprise with ESI, LSCache, async SSL, .htaccess support, and 16 upstream bug fixes. The best free alternative to LiteSpeed Web Server for WordPress, Magento, Laravel, and shared hosting.

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Base Version](https://img.shields.io/badge/Base-OLS%201.8.5-green.svg)](https://openlitespeed.org/)
[![Enterprise Features](https://img.shields.io/badge/Features-11%20Enterprise-orange.svg)](#enterprise-features)
[![Upstream Fixes](https://img.shields.io/badge/Upstream%20Fixes-16%20Issues-brightgreen.svg)](#upstream-issues-fixed)

**Keywords:** OpenLiteSpeed, LiteSpeed Enterprise, LiteSpeed Web Server, OLS, LSWS, LSCache, ESI, HTTP/3, QUIC, WordPress hosting, high performance web server, Apache alternative, Nginx alternative, shared hosting, WHM, cPanel, free web server, open source web server, PHP hosting, LSAPI

---

## No Limits. No License. No Restrictions.

This fork removes **all artificial limits** from OpenLiteSpeed and adds enterprise features that normally require a paid LiteSpeed Enterprise license.

| Resource | Stock OLS | This Fork |
|----------|-----------|-----------|
| Domains / VHosts | Unlimited | Unlimited |
| Max Connections | 1,000,000 cap | **Unlimited** (INT_MAX) |
| Max SSL Connections | 1,000,000 cap | **Unlimited** (INT_MAX) |
| Default Connections | 2,000 | **100,000** |
| Workers (FCGI) | 2,000 max | **50,000** |
| Worker Connections | 10,000 per worker | **100,000** |
| LSAPI PHP Children | 5 per user | **100** per user |
| Per-Client Connections | 100 hard limit | **10,000** |
| LSCache Object Size | 10 MB | **256 MB** |
| LSCache Stale TTL | 200 seconds | **86,400** (24 hours) |
| Keep-Alive Requests | 100 per connection | **10,000** |
| PHP Configs per VHost | 100 | **1,000** |
| Access Logs per VHost | 4 | **32** |
| Memory | No artificial cap | No artificial cap |
| Listeners | Unlimited | Unlimited |
| LSCache Features | Full | Full + Crawler enabled |
| License Checks | None | None |
| `_ENTERPRISE_` Gate | Server header gated | **Removed** |

See [docs/13-limits-removed.md](docs/13-limits-removed.md) for the complete breakdown and tuning guide.

---

## Quick Start

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install -y build-essential cmake git libssl-dev libpcre3-dev \
    zlib1g-dev libxml2-dev libhiredis-dev libbrotli-dev

# Clone, build, install
git clone https://github.com/kumaraguru1735/openlitespeed-enterprise.git
cd openlitespeed-enterprise
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local/lsws
make -j$(nproc)
sudo make install

# Setup admin and start
sudo /usr/local/lsws/admin/misc/admpass.sh
sudo /usr/local/lsws/bin/lswsctrl start

# Verify
curl -I http://localhost:8088
```

See [docs/01-installation.md](docs/01-installation.md) for detailed instructions for all platforms.

---

## Enterprise Features

11 enterprise features that normally require LiteSpeed Enterprise ($0-$65/mo license):

| # | Feature | Guide |
|---|---------|-------|
| 1 | **Full .htaccess Support** - Apache-compatible with auto-reload (2s) | [docs/03-htaccess-guide.md](docs/03-htaccess-guide.md) |
| 2 | **ESI (Edge Side Includes)** - Fragment caching for dynamic pages | [docs/04-esi-guide.md](docs/04-esi-guide.md) |
| 3 | **Async SSL Handshake** - Offload TLS to worker threads | [docs/08-ssl-async-handshake.md](docs/08-ssl-async-handshake.md) |
| 4 | **WordPress Protection** - Brute force + reCAPTCHA | [docs/06-wordpress-protection.md](docs/06-wordpress-protection.md) |
| 5 | **JIT VHost Loading** - Lazy config for fast restarts | [docs/09-jit-vhost-loading.md](docs/09-jit-vhost-loading.md) |
| 6 | **Redis Dynamic VHost** - Mass hosting via Redis | [docs/07-redis-dynamic-vhost.md](docs/07-redis-dynamic-vhost.md) |
| 7 | **Async ModSecurity WAF** - Non-blocking rule evaluation | [docs/12-modsecurity-async.md](docs/12-modsecurity-async.md) |
| 8 | **CPU Affinity** - Pin workers to CPU cores | [docs/14-cpu-affinity.md](docs/14-cpu-affinity.md) |
| 9 | **VHost Bandwidth Throttling** - Per-vhost aggregate limits | [docs/15-vhost-bandwidth-throttling.md](docs/15-vhost-bandwidth-throttling.md) |
| 10 | **Anti-DDoS + reCAPTCHA** - Smart escalation pipeline | [docs/10-anti-ddos-guide.md](docs/10-anti-ddos-guide.md) |
| 11 | **LSAPI Daemon Mode** - Persistent PHP per user | [docs/11-lsapi-daemon-mode.md](docs/11-lsapi-daemon-mode.md) |

### Quick Enable

```apacheconf
# Add these to /usr/local/lsws/conf/httpd_config.conf

sslAsyncHandshake         1       # Async SSL
cpuAffinityMode           1       # Auto CPU pinning
jitVHost                  1       # Lazy vhost loading
antiDdosCaptcha           1       # Smart DDoS protection

module mod_wpprotect {
    wpProtect             1
    wpProtectMaxRetry     5
    wpProtectAction       1       # reCAPTCHA challenge
    recaptchaSiteKey      YOUR_KEY
    recaptchaSecretKey    YOUR_SECRET
}

module mod_security {
    modsecurity           1
    modsecAsync           1       # Async WAF
}
```

---

## LSCache - Fully Unlocked

LSCache is the server-level page cache that makes LiteSpeed fast. In this fork it's fully unlocked with enterprise-grade defaults:

- **256 MB** max object size (was 10 MB)
- **24-hour** max stale TTL (was 200 seconds)
- **Cache crawler** enabled (`X-LSCACHE: on,crawler`)
- **Public + Private** cache both fully functional
- **No edition gating** - all features available

```php
// Enable LSCache in your PHP app:
header('X-LiteSpeed-Cache-Control: public, max-age=3600');
header('X-LiteSpeed-Tag: product_123');

// ESI support:
header('X-LiteSpeed-Cache-Control: public, max-age=3600, esi=on');
```

See [docs/05-lscache-guide.md](docs/05-lscache-guide.md) for the complete guide.

---

## Upstream Issues Fixed

16 bugs from the [official OLS issue tracker](https://github.com/litespeedtech/openlitespeed/issues) are fixed in this fork:

| # | Issue | Category |
|---|-------|----------|
| [#474](https://github.com/litespeedtech/openlitespeed/issues/474) | .htaccess Apache compatibility | Feature |
| [#470](https://github.com/litespeedtech/openlitespeed/issues/470) | OLS artificially limited ("crippled") | Limits |
| [#414](https://github.com/litespeedtech/openlitespeed/issues/414) | ESI support | Feature |
| [#460](https://github.com/litespeedtech/openlitespeed/issues/460) | Server-level error pages not inherited | Bug |
| [#453](https://github.com/litespeedtech/openlitespeed/issues/453) | System env vars not passed to LSAPI | Bug |
| [#439](https://github.com/litespeedtech/openlitespeed/issues/439) | ACL allow/deny lists broken | Bug |
| [#436](https://github.com/litespeedtech/openlitespeed/issues/436) | Env var expansion in Header Operations | Bug |
| [#434](https://github.com/litespeedtech/openlitespeed/issues/434) | Null bytes in URIs (security) | Security |
| [#412](https://github.com/litespeedtech/openlitespeed/issues/412) | PHP fails without LSAPI_CHILDREN | Bug |
| [#409](https://github.com/litespeedtech/openlitespeed/issues/409) | Rails default env overrides custom | Bug |
| [#402](https://github.com/litespeedtech/openlitespeed/issues/402) | Pipelined requests after chunked body | Bug |
| [#394](https://github.com/litespeedtech/openlitespeed/issues/394) | Request smuggling via bare LF (security) | Security |
| [#384](https://github.com/litespeedtech/openlitespeed/issues/384) | Wrong Content-Type for .js.gz files | Bug |
| [#332](https://github.com/litespeedtech/openlitespeed/issues/332) | OCSP stapling fails after cert renewal | Bug |
| [#316](https://github.com/litespeedtech/openlitespeed/issues/316) | Cache swap files fill disk | Bug |
| [#265](https://github.com/litespeedtech/openlitespeed/issues/265) | Double gzip on proxy responses | Bug |

---

## Feature Comparison

| Feature | Stock OLS | LiteSpeed Enterprise | This Fork |
|---------|-----------|---------------------|-----------|
| HTTP/2 & HTTP/3 (QUIC) | Yes | Yes | Yes |
| LSCache (public/private) | Yes | Yes | Yes (unlocked) |
| PageSpeed Module | Yes | Yes | Yes |
| ModSecurity WAF | Sync only | Async | **Async** |
| ESI (Edge Side Includes) | No | Yes | **Yes** |
| Full .htaccess | No | Yes | **Yes** |
| .htaccess Auto-Reload | No | Yes | **Yes (2s)** |
| Async SSL Handshake | No | Yes | **Yes** |
| WP Brute Force | No | Yes | **Yes** |
| JIT VHost Loading | No | Yes | **Yes** |
| Redis Dynamic VHost | No | Yes | **Yes** |
| CPU Affinity | Basic | Yes | **Yes** |
| VHost Bandwidth Throttle | No | Yes | **Yes** |
| Anti-DDoS + reCAPTCHA | Separate | Integrated | **Integrated** |
| LSAPI Daemon Mode | No | Yes | **Yes** |
| Connection Limits | 1M cap | Unlimited | **Unlimited** |
| License Required | No | Yes ($) | **No** |

---

## Documentation

Complete documentation is in the [docs/](docs/) folder:

| Guide | Description |
|-------|-------------|
| [Installation](docs/01-installation.md) | Build from source, systemd setup, all platforms |
| [Configuration Basics](docs/02-configuration-basics.md) | Config files, first vhost, PHP, SSL |
| [.htaccess Guide](docs/03-htaccess-guide.md) | All supported directives with examples |
| [ESI Guide](docs/04-esi-guide.md) | Fragment caching with include/remove/try tags |
| [LSCache Guide](docs/05-lscache-guide.md) | Page cache, purge, crawler, WordPress |
| [WordPress Protection](docs/06-wordpress-protection.md) | Brute force + reCAPTCHA setup |
| [Redis Dynamic VHost](docs/07-redis-dynamic-vhost.md) | Mass hosting via Redis lookup |
| [SSL Async Handshake](docs/08-ssl-async-handshake.md) | Thread pool TLS offloading |
| [JIT VHost Loading](docs/09-jit-vhost-loading.md) | Lazy config for fast restarts |
| [Anti-DDoS Guide](docs/10-anti-ddos-guide.md) | Smart escalation with reCAPTCHA |
| [LSAPI Daemon Mode](docs/11-lsapi-daemon-mode.md) | Persistent PHP processes |
| [ModSecurity Async](docs/12-modsecurity-async.md) | Non-blocking WAF processing |
| [Limits Removed](docs/13-limits-removed.md) | Complete list of raised limits |
| [CPU Affinity](docs/14-cpu-affinity.md) | Worker-to-core pinning |
| [VHost Bandwidth](docs/15-vhost-bandwidth-throttling.md) | Per-vhost traffic shaping |
| [Troubleshooting](docs/16-troubleshooting.md) | Common issues and solutions |

---

## systemd Service

```bash
# Create service file
sudo tee /etc/systemd/system/lsws-enterprise.service << 'EOF'
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
EOF

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

GNU General Public License v3.0 - Free to use, modify, and distribute.

Copyright (C) 2013-2026 LiteSpeed Technologies, Inc.
Enterprise additions Copyright (C) 2026 kumaraguru1735.
