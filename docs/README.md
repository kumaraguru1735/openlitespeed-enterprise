# Documentation Index

Complete guide collection for OpenLiteSpeed configuration, optimization, and troubleshooting. Each guide is beginner-friendly with real-world examples.

## Guides

| # | Guide | Description |
|---|-------|-------------|
| 1 | [Installation](01-installation.md) | How to install OpenLiteSpeed from packages or source on various Linux distributions. |
| 2 | [Configuration Basics](02-configuration-basics.md) | Core server configuration, listeners, virtual hosts, and essential tuning parameters. |
| 3 | [.htaccess Guide](03-htaccess.md) | Using .htaccess files with OpenLiteSpeed for rewrites, redirects, and access control. |
| 4 | [ESI Guide](04-esi.md) | Edge Side Includes for partial page caching and dynamic content assembly. |
| 5 | [LSCache Guide](05-lscache.md) | Built-in page caching with LiteSpeed Cache for dramatically faster response times. |
| 6 | [WordPress Protection](06-wordpress-protection.md) | Securing WordPress installations against brute force, spam, and common attacks. |
| 7 | [Redis Dynamic VHost](07-redis-dynamic-vhost.md) | Dynamically resolving virtual host configurations from Redis for scalable multi-tenant hosting. |
| 8 | [SSL Async Handshake](08-ssl-async-handshake.md) | Non-blocking SSL/TLS handshakes to maintain throughput under heavy HTTPS traffic. |
| 9 | [JIT VHost Loading](09-jit-vhost-loading.md) | Just-in-time virtual host loading to reduce memory usage on servers with many sites. |
| 10 | [Anti-DDoS Protection](10-anti-ddos.md) | Built-in connection and request rate limiting to mitigate denial-of-service attacks. |
| 11 | [LSAPI Daemon Mode](11-lsapi-daemon-mode.md) | Running PHP via LSAPI in daemon mode for persistent processes and lower overhead. |
| 12 | [ModSecurity Async WAF](12-modsecurity-async-waf.md) | Asynchronous Web Application Firewall processing with ModSecurity and OWASP rules. |
| 13 | [Limits Removed](13-limits-removed.md) | Understanding and configuring connection, request, and resource limits in OpenLiteSpeed. |
| 14 | [CPU Affinity](14-cpu-affinity.md) | Pinning worker processes to CPU cores for better cache locality and consistent performance. |
| 15 | [VHost Bandwidth Throttling](15-vhost-bandwidth-throttling.md) | Per-vhost bandwidth limits for fair resource allocation on shared hosting servers. |
| 16 | [Troubleshooting](16-troubleshooting.md) | Diagnosing and fixing common issues including startup failures, error codes, SSL, caching, and performance. |

## Quick Start

New to OpenLiteSpeed? Read the guides in order:

1. Start with **Installation** to get OLS running
2. Read **Configuration Basics** to understand the config file structure
3. Set up **SSL Async Handshake** for HTTPS
4. Enable **LSCache** for page caching
5. Review **Troubleshooting** when something goes wrong

## For Shared Hosting Providers

If you are setting up a shared hosting server, these guides are especially relevant:

- **Redis Dynamic VHost** -- Manage hundreds of vhosts without config file sprawl
- **JIT VHost Loading** -- Keep memory usage low with many sites
- **VHost Bandwidth Throttling** -- Prevent any single site from monopolizing bandwidth
- **Anti-DDoS Protection** -- Protect the server from abusive traffic
- **Limits Removed** -- Understand and tune connection limits
- **CPU Affinity** -- Get the best performance from your hardware

## For WordPress Hosting

- **LSCache Guide** -- The single biggest performance improvement for WordPress
- **WordPress Protection** -- Block brute force and common WordPress attacks
- **LSAPI Daemon Mode** -- Faster PHP execution for WordPress
- **Troubleshooting** -- Fix common WordPress + OLS issues
