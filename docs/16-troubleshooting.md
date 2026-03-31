# Troubleshooting Guide

This guide covers the most common OpenLiteSpeed problems, how to diagnose them, and how to fix them. Each section follows the same pattern: symptom, diagnosis steps, and solution.

## Server Won't Start

### Port Already in Use

**Symptom:** OLS fails to start. Error log says `Address already in use`.

**Diagnose:**

```bash
# Check what is using port 80 and 443
ss -tlnp | grep -E ':80|:443'

# Or with lsof
lsof -i :80
lsof -i :443
```

**Fix:**

```bash
# If Apache is running, stop it
systemctl stop apache2
systemctl disable apache2

# If nginx is running, stop it
systemctl stop nginx
systemctl disable nginx

# If another OLS instance is stuck
kill $(cat /tmp/lshttpd/lshttpd.pid)

# Then start OLS
systemctl start lsws
```

### Configuration Errors

**Symptom:** OLS exits immediately after starting. Error log shows syntax errors.

**Diagnose:**

```bash
# Check the error log for config parsing failures
tail -50 /usr/local/lsws/logs/error.log

# Test the configuration without starting
/usr/local/lsws/bin/openlitespeed -t
```

**Common config mistakes:**

```
# Missing closing brace
virtualHost example.com {
    vhRoot /var/www/example.com
    # forgot the closing }

# Wrong directive name (case-sensitive)
DocumentRoot /var/www/html      # Wrong - Apache syntax
docRoot      /var/www/html      # Correct - OLS syntax

# Invalid listener binding
listener HTTP {
    address     *:80
    # Missing closing brace or invalid IP
}
```

**Fix:** Correct the syntax error shown in the log, then restart:

```bash
systemctl restart lsws
```

### Permission Errors

**Symptom:** OLS starts but cannot bind to ports below 1024, or cannot read config files.

**Diagnose:**

```bash
# Check OLS process user
ps aux | grep litespeed

# Check config file permissions
ls -la /usr/local/lsws/conf/httpd_config.conf

# Check if OLS is trying to run as non-root on privileged ports
tail -20 /usr/local/lsws/logs/error.log | grep -i perm
```

**Fix:**

```bash
# OLS must start as root to bind to ports 80/443
# It will drop privileges to the configured user afterward
systemctl start lsws

# Fix config file permissions if needed
chown -R lsadm:lsadm /usr/local/lsws/conf/
chmod 644 /usr/local/lsws/conf/httpd_config.conf
```

## 503 Service Unavailable Errors

A 503 means OLS received the request but the backend (usually PHP) could not handle it.

### PHP-FPM / LSPHP Not Running

**Diagnose:**

```bash
# Check if LSPHP processes exist
ps aux | grep lsphp

# Check if PHP-FPM is running (if using external app)
systemctl status php8.2-fpm

# Check the OLS error log for backend errors
tail -30 /usr/local/lsws/logs/error.log | grep -i "503\|backend\|lsphp"
```

**Fix:**

```bash
# Restart LSPHP (OLS manages it automatically, so restart OLS)
systemctl restart lsws

# If using external PHP-FPM
systemctl restart php8.2-fpm
```

### External App Timeout

**Symptom:** Pages hang for 30-60 seconds then show 503.

**Diagnose:**

```bash
# Check if PHP scripts are running too long
tail -50 /usr/local/lsws/logs/error.log | grep -i timeout

# Check PHP-FPM status
curl -s http://localhost/fpm-status 2>/dev/null || echo "FPM status page not enabled"

# Check for stuck PHP processes
ps aux | grep php | grep -v grep | wc -l
```

**Fix:**

Increase the external app timeout in your vhost config or server config:

```xml
extProcessor lsphp {
    type                lsapi
    address             uds://tmp/lshttpd/lsphp.sock
    maxConns            20
    env                 PHP_LSAPI_CHILDREN=20
    initTimeout         60       # Seconds to wait for first response
    retryTimeout        0
    respBuffer          0
    backlog             100
    connTimeout         60
}
```

If PHP scripts legitimately need more time (e.g., imports, migrations), also increase PHP's `max_execution_time`:

```bash
# In php.ini
max_execution_time = 300
```

### Too Few PHP Workers

**Symptom:** 503 errors under load. Works fine with low traffic.

**Diagnose:**

```bash
# Count active PHP processes
ps aux | grep lsphp | grep -v grep | wc -l

# Check OLS error log for "max children" or "no available" messages
grep -i "max children\|no available\|too many" /usr/local/lsws/logs/error.log | tail -10
```

**Fix:** Increase PHP workers:

```xml
extProcessor lsphp {
    type                lsapi
    maxConns            50            # Increase from default
    env                 PHP_LSAPI_CHILDREN=50
}
```

## 500 Internal Server Error

### PHP Fatal Errors

**Diagnose:**

```bash
# Check PHP error log
tail -30 /usr/local/lsws/logs/error.log

# Check site-specific PHP error log if configured
tail -30 /var/www/example.com/logs/php_error.log

# Enable PHP error display temporarily for debugging
# In php.ini or .htaccess:
# display_errors = On
# error_reporting = E_ALL
```

**Common causes:**

```
# Missing PHP extension
Fatal error: Call to undefined function mysqli_connect()
Fix: apt install php8.2-mysql && systemctl restart lsws

# Out of memory
Fatal error: Allowed memory size of 134217728 bytes exhausted
Fix: Increase memory_limit in php.ini

# File permission error
Fatal error: Failed opening required '/var/www/site/config.php'
Fix: chown -R www-data:www-data /var/www/site/
```

### .htaccess Syntax Errors

**Diagnose:**

```bash
# Check OLS error log for rewrite errors
grep -i "htaccess\|rewrite" /usr/local/lsws/logs/error.log | tail -10

# Look for common .htaccess problems
cat /var/www/example.com/public_html/.htaccess
```

**Common .htaccess mistakes:**

```apache
# Using Apache-only directives that OLS doesn't support
<IfModule mod_headers.c>
    # OLS supports this, but some directives inside may not work
</IfModule>

# Missing RewriteEngine On
RewriteRule ^(.*)$ /index.php/$1 [L]
# Fix: Add "RewriteEngine On" before any RewriteRule

# Invalid regex in RewriteRule
RewriteRule ^[invalid( /index.php [L]
# Fix: Correct the regular expression
```

**Fix:** Correct the .htaccess file. To quickly test if .htaccess is the problem:

```bash
# Temporarily rename .htaccess
mv /var/www/example.com/public_html/.htaccess /var/www/example.com/public_html/.htaccess.bak

# If the site works now, the problem is in .htaccess
# Restore and fix the specific issue
mv /var/www/example.com/public_html/.htaccess.bak /var/www/example.com/public_html/.htaccess
```

## 404 Not Found Errors

### Wrong Document Root

**Diagnose:**

```bash
# Check what docRoot is configured for the vhost
grep -A5 "virtualHost example.com" /usr/local/lsws/conf/httpd_config.conf
grep "docRoot" /usr/local/lsws/conf/vhosts/example.com/vhconf.conf

# Check if the directory actually exists
ls -la /var/www/example.com/public_html/
```

**Fix:** Make sure the `docRoot` in the vhost config matches the actual directory:

```xml
# In vhconf.conf
docRoot     $VH_ROOT/public_html/
```

And that `vhRoot` in the main config is correct:

```xml
virtualHost example.com {
    vhRoot      /var/www/example.com
}
```

### Rewrite Rules Not Working

**Symptom:** Direct file URLs work (`/about.html`) but clean URLs don't (`/about`).

**Diagnose:**

```bash
# Check if rewrite is enabled in vhost config
grep -A10 "rewrite" /usr/local/lsws/conf/vhosts/example.com/vhconf.conf

# Check .htaccess for rewrite rules
cat /var/www/example.com/public_html/.htaccess
```

**Fix:** Enable rewrites in the vhost config:

```xml
rewrite {
    enable          1
    autoLoadHtaccess 1
}
```

Or add rewrite rules directly:

```xml
rewrite {
    enable          1
    rules           <<<END_rules
    RewriteEngine On
    RewriteCond %{REQUEST_FILENAME} !-f
    RewriteCond %{REQUEST_FILENAME} !-d
    RewriteRule ^(.*)$ /index.php/$1 [L]
    END_rules
}
```

## SSL Certificate Issues

### Certificate Not Found or Expired

**Diagnose:**

```bash
# Check certificate expiry
openssl x509 -in /etc/letsencrypt/live/example.com/fullchain.pem -noout -enddate

# Test SSL connection
openssl s_client -connect example.com:443 -servername example.com </dev/null 2>/dev/null | openssl x509 -noout -dates

# Check OLS error log for SSL errors
grep -i "ssl\|cert\|tls" /usr/local/lsws/logs/error.log | tail -10
```

**Fix:**

```bash
# Renew with certbot
certbot renew

# Or issue a new certificate
certbot certonly --webroot -w /var/www/example.com/public_html -d example.com -d www.example.com

# Make sure the vhost points to the right cert files
```

In the listener config:

```xml
listener HTTPS {
    address             *:443
    secure              1
    keyFile             /etc/letsencrypt/live/example.com/privkey.pem
    certFile            /etc/letsencrypt/live/example.com/fullchain.pem
}
```

### OCSP Stapling Issues

**Symptom:** Slow initial SSL handshakes. Browser shows "Revocation information not available."

**Diagnose:**

```bash
# Test OCSP stapling
openssl s_client -connect example.com:443 -servername example.com -status </dev/null 2>/dev/null | grep -A5 "OCSP"
```

**Fix:** Enable OCSP stapling in the listener SSL settings:

```xml
listener HTTPS {
    address             *:443
    secure              1
    keyFile             /etc/letsencrypt/live/example.com/privkey.pem
    certFile            /etc/letsencrypt/live/example.com/fullchain.pem
    enableStapling      1
    ocspRespMaxAge      86400
}
```

### Auto-Renewal Not Working

**Diagnose:**

```bash
# Test certbot renewal
certbot renew --dry-run

# Check certbot timer
systemctl status certbot.timer

# Check if the webroot is accessible
curl -I http://example.com/.well-known/acme-challenge/test
```

**Fix:**

```bash
# Make sure the .well-known directory is accessible
# In .htaccess or vhost config, do NOT block this path
# Add an exception if you have blanket deny rules:

# .htaccess
RewriteRule ^\.well-known/ - [L]

# Enable and start the certbot timer
systemctl enable certbot.timer
systemctl start certbot.timer

# Add a post-renewal hook to restart OLS
echo '#!/bin/bash
systemctl restart lsws' > /etc/letsencrypt/renewal-hooks/deploy/restart-ols.sh
chmod +x /etc/letsencrypt/renewal-hooks/deploy/restart-ols.sh
```

## Cache Not Working

### Checking Cache Headers

```bash
# Check response headers for cache status
curl -I https://example.com/ 2>/dev/null | grep -i "x-litespeed\|cache\|age"

# Expected output when cache is working:
# X-LiteSpeed-Cache: hit
# Age: 120

# When cache misses:
# X-LiteSpeed-Cache: miss
```

### Cache Not Populating

**Diagnose:**

```bash
# Check if the cache module is loaded
grep -i "cache" /usr/local/lsws/conf/httpd_config.conf

# Check if cache is enabled for the vhost
grep -i "cache" /usr/local/lsws/conf/vhosts/example.com/vhconf.conf

# Check cache directory permissions
ls -la /usr/local/lsws/cachedata/
```

**Fix:**

Enable cache at the server level:

```xml
module cache {
    enableCache         1
    cacheStorePath      /usr/local/lsws/cachedata/
    checkPrivateCache   1
    checkPublicCache    1
}
```

Enable cache in the vhost config:

```xml
context / {
    enableCache         1
    cacheExpire         86400
}
```

### Purging the Cache

```bash
# Purge all cached content
rm -rf /usr/local/lsws/cachedata/*

# Purge cache for a specific vhost
rm -rf /usr/local/lsws/cachedata/example.com/

# Purge via OLS admin console
# Navigate to: Server > Cache Manager > Purge All
```

### Debugging Cache Issues

Add these response headers temporarily to see why pages are not being cached:

```xml
# In vhost config
module cache {
    enableCache     1
    cacheStorePath  /usr/local/lsws/cachedata/
    # Enable cache debug headers
}
```

Check for reasons pages might not be cached:
- Response has `Set-Cookie` header (logged-in users)
- Response has `Cache-Control: no-cache` or `private`
- POST requests are never cached
- Response code is not 200
- Response body is empty

## PHP Performance Issues

### Too Few Workers

**Symptom:** Slow response times under load. PHP processes maxed out.

**Diagnose:**

```bash
# Count PHP processes
ps aux | grep lsphp | grep -v grep | wc -l

# Check if max children is being hit
grep -i "max children\|reached" /usr/local/lsws/logs/error.log | tail -5

# Check PHP-FPM pool status (if enabled)
curl -s http://localhost/fpm-status
```

**Fix:** Increase worker count:

```xml
extProcessor lsphp {
    type            lsapi
    maxConns        50
    env             PHP_LSAPI_CHILDREN=50
    env             PHP_LSAPI_MAX_REQUESTS=1000
}
```

Rule of thumb for setting PHP workers:

```
Available RAM - OS overhead (512 MB) - OLS overhead (256 MB)
÷ Average PHP process memory (40-80 MB)
= Maximum PHP workers

Example: 4 GB server
(4096 - 512 - 256) / 60 = ~55 workers
```

### Memory Issues

**Diagnose:**

```bash
# Check per-process PHP memory usage
ps aux --sort=-%mem | grep lsphp | head -10

# Check PHP memory limit
php -i | grep memory_limit

# Check system memory
free -h
```

**Fix:**

```bash
# In php.ini
memory_limit = 256M     # Increase if needed, but watch total RAM

# For WordPress specifically
# In wp-config.php
define('WP_MEMORY_LIMIT', '256M');
```

## High CPU Usage

### Identifying the Cause

```bash
# Check which processes use the most CPU
top -bn1 | head -20

# Check if it's OLS or PHP
ps aux --sort=-%cpu | grep -E "litespeed|lsphp" | head -10

# Check OLS server load
curl -s http://localhost:7080/status 2>/dev/null

# Profile PHP if it's the culprit
# Enable slow log in php.ini
# slowlog = /var/log/php-slow.log
# request_slowlog_timeout = 5s
```

### OLS Using High CPU

Possible causes:
- Too many concurrent connections with SSL
- Excessive rewrite rule processing
- Large access logs being written synchronously

**Fix:**

```xml
# Reduce logging verbosity
logLevel                0    # Error only (0=error, 1=warning, 2=notice, 3=info, 9=debug)

# Enable async access log
accessLog {
    fileName            /usr/local/lsws/logs/access.log
    pipedLogger         1
}
```

### PHP Using High CPU

```bash
# Find the slow PHP script
# Enable the PHP slow log
# In php.ini or pool config:
# slowlog = /var/log/php/slow.log
# request_slowlog_timeout = 3

# Check the slow log
tail -50 /var/log/php/slow.log

# Common causes:
# - Unoptimized database queries (add indexes)
# - Missing opcode cache (install opcache)
# - Large file processing in PHP
# - Infinite loops in code
```

Ensure OPcache is enabled:

```bash
# Check if OPcache is active
php -i | grep opcache.enable

# Enable in php.ini if not
opcache.enable=1
opcache.memory_consumption=128
opcache.max_accelerated_files=10000
opcache.revalidate_freq=2
```

## Log File Locations

### OpenLiteSpeed Logs

| Log | Path | Contains |
|-----|------|----------|
| Error log | `/usr/local/lsws/logs/error.log` | Server errors, config errors, warnings |
| Access log | `/usr/local/lsws/logs/access.log` | All HTTP requests |
| Stderr log | `/usr/local/lsws/logs/stderr.log` | PHP and external app errors |
| Admin error log | `/usr/local/lsws/admin/logs/error.log` | Admin panel errors |

### Per-VHost Logs

If configured, each vhost can have its own logs:

```xml
# In vhconf.conf
errorLog {
    fileName    $VH_ROOT/logs/error.log
    logLevel    2
}

accessLog {
    fileName    $VH_ROOT/logs/access.log
}
```

### PHP Logs

| Log | Path | Contains |
|-----|------|----------|
| PHP error log | `/var/log/php/error.log` (varies) | PHP runtime errors |
| PHP-FPM log | `/var/log/php8.2-fpm.log` | FPM pool manager errors |
| PHP slow log | `/var/log/php/slow.log` | Scripts exceeding timeout |

### How to Read OLS Error Logs

```bash
# Show recent errors
tail -50 /usr/local/lsws/logs/error.log

# Filter by severity
grep "\[ERROR\]" /usr/local/lsws/logs/error.log | tail -20
grep "\[WARN\]" /usr/local/lsws/logs/error.log | tail -20

# Filter by vhost
grep "example.com" /usr/local/lsws/logs/error.log | tail -20

# Follow log in real time (useful while reproducing issues)
tail -f /usr/local/lsws/logs/error.log

# Check for errors in the last hour
awk '/2026-03-31 1[0-9]:/' /usr/local/lsws/logs/error.log
```

### Increasing Log Verbosity

Temporarily increase verbosity to debug an issue:

```xml
# In httpd_config.conf
# 0=Error, 1=Warning, 2=Notice, 3=Info, 9=Debug
logLevel        9    # Maximum detail (use temporarily!)
debugLevel      10   # OLS internal debug (very verbose)
```

**Important:** Reset to `logLevel 0` and `debugLevel 0` after debugging. Debug logging generates enormous log files and impacts performance.

## Common Config Mistakes

### 1. Using Apache Directive Names

```xml
# Wrong (Apache syntax)
DocumentRoot /var/www/html
ServerName example.com

# Correct (OLS syntax)
docRoot     $VH_ROOT/public_html/
# ServerName is set in the virtualHost block name and listener mapping
```

### 2. Forgetting to Map VHost to Listener

A virtual host must be mapped to a listener, or it will never receive requests:

```xml
listener HTTP {
    address     *:80
    map         example.com example.com
}

listener HTTPS {
    address     *:443
    secure      1
    map         example.com example.com
}
```

### 3. External App Socket Permissions

If OLS cannot connect to a PHP socket:

```bash
# Check socket exists
ls -la /tmp/lshttpd/

# Check socket permissions
ls -la /tmp/lshttpd/lsphp.sock

# Fix permissions
chown nobody:nobody /tmp/lshttpd/lsphp.sock
chmod 660 /tmp/lshttpd/lsphp.sock
```

### 4. File Permissions Too Open or Too Strict

```bash
# Too open (security risk)
chmod 777 /var/www/site/     # Never do this

# Too strict (OLS can't read files)
chmod 600 /var/www/site/     # OLS user can't access

# Correct
chown -R www-data:www-data /var/www/site/
find /var/www/site/ -type d -exec chmod 755 {} \;
find /var/www/site/ -type f -exec chmod 644 {} \;
```

### 5. Forgetting to Restart After Config Changes

OLS does not auto-reload config files. Always restart or gracefully reload after changes:

```bash
# Graceful restart (no downtime)
/usr/local/lsws/bin/lswsctrl restart

# Or via systemctl
systemctl restart lsws

# To reload without full restart (limited to some changes)
kill -USR1 $(cat /tmp/lshttpd/lshttpd.pid)
```

### 6. SSL Listener Without Certificate

```xml
# This will fail - secure listener needs cert files
listener HTTPS {
    address     *:443
    secure      1
    # Missing keyFile and certFile!
}

# Correct
listener HTTPS {
    address     *:443
    secure      1
    keyFile     /etc/letsencrypt/live/example.com/privkey.pem
    certFile    /etc/letsencrypt/live/example.com/fullchain.pem
}
```

## Quick Diagnostic Checklist

When something is wrong, run through this checklist:

```bash
# 1. Is OLS running?
systemctl status lsws

# 2. Any errors in the log?
tail -30 /usr/local/lsws/logs/error.log

# 3. Is the port open?
ss -tlnp | grep -E ':80|:443'

# 4. Is PHP running?
ps aux | grep lsphp | grep -v grep

# 5. Can you reach the server locally?
curl -I http://localhost/

# 6. Is DNS pointing to this server?
dig +short example.com

# 7. Is the firewall allowing traffic?
ufw status
# or
iptables -L -n | grep -E ':80|:443'

# 8. Disk space OK?
df -h /

# 9. Memory OK?
free -h

# 10. Config valid?
/usr/local/lsws/bin/openlitespeed -t
```

## How to Get Help

### Community Resources

- **OpenLiteSpeed Community:** [https://openlitespeed.org/community/](https://openlitespeed.org/community/)
- **GitHub Issues:** [https://github.com/litespeedtech/openlitespeed/issues](https://github.com/litespeedtech/openlitespeed/issues)
- **Official Documentation:** [https://openlitespeed.org/kb/](https://openlitespeed.org/kb/)

### When Filing a Bug Report

Include the following information:

```bash
# OLS version
/usr/local/lsws/bin/openlitespeed -v

# OS version
cat /etc/os-release

# Error log excerpt (relevant portion)
tail -100 /usr/local/lsws/logs/error.log

# Server config (remove sensitive data like IPs and paths)
cat /usr/local/lsws/conf/httpd_config.conf

# Steps to reproduce the issue
```

### Before Asking for Help

1. Check the error log -- 90% of issues are explained there
2. Search the community forum -- your issue has likely been solved before
3. Test with a minimal config -- remove custom rules to isolate the problem
4. Try a graceful restart -- some issues are transient
5. Check this troubleshooting guide -- the answer may be here
