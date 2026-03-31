# Redis Dynamic Virtual Hosting Guide

## What Is Redis VHost?

Redis VHost is a module for OpenLiteSpeed that dynamically resolves virtual host configurations from a Redis database instead of static configuration files. When a request arrives, OLS queries Redis with the domain name to get the document root, PHP settings, and other configuration -- all without restarting the server.

### Who Needs This?

- **Hosting providers** managing hundreds or thousands of domains
- **SaaS platforms** where each customer has their own domain
- **Auto-scaling environments** where domains are added/removed frequently
- **Multi-tenant applications** that map many domains to different directories

Without Redis VHost, adding a new domain requires editing OLS configuration files and performing a graceful restart. With Redis VHost, you simply insert a record into Redis, and the next request for that domain is routed automatically.

## Architecture

Here is how a request flows through the Redis VHost system:

```
1. Client requests https://customer-site.com/
         |
2. OLS receives the request, extracts hostname "customer-site.com"
         |
3. OLS checks its in-memory cache for this hostname
         |
    [Cache HIT] --> Return cached config --> Serve request
         |
    [Cache MISS] --> Query Redis: GET "vhost:customer-site.com"
         |
4. Redis returns JSON config (docroot, PHP version, etc.)
         |
5. OLS caches the config in memory (for TTL seconds)
         |
6. OLS serves the request using the resolved configuration
```

The in-memory cache layer means Redis is only queried once per domain per TTL period, keeping performance high even with thousands of domains.

## Prerequisites

### Install Redis

```bash
# Ubuntu/Debian
apt update && apt install redis-server -y

# CentOS/RHEL
yum install redis -y

# Start and enable Redis
systemctl enable redis
systemctl start redis

# Verify Redis is running
redis-cli ping
# Should return: PONG
```

### Install hiredis Library

The hiredis C library is required for OLS to communicate with Redis:

```bash
# Ubuntu/Debian
apt install libhiredis-dev -y

# CentOS/RHEL
yum install hiredis-devel -y
```

### Verify Library Installation

```bash
ldconfig -p | grep hiredis
# Should show: libhiredis.so.X => /usr/lib/x86_64-linux-gnu/libhiredis.so.X
```

## Building OLS with Redis VHost Support

This module must be enabled at compile time. When building OLS from source, add the flag:

```bash
cmake -DMOD_REDISVHOST=ON ..
```

If you are using this project's build system, the module is included when you run:

```bash
./build_ols.sh
```

Check that the module binary exists after building:

```bash
ls -la /usr/local/lsws/modules/mod_redisvhost.so
```

## Configuration

Add the module configuration to your OLS server configuration (httpd_config.conf):

```
module mod_redisvhost {
    enabled             1

    # Redis connection
    redisHost           127.0.0.1
    redisPort           6379
    redisAuth           your_redis_password_here
    redisDb             0

    # Key prefix -- domain name is appended to this
    redisKeyPrefix      vhost:

    # Cache TTL in seconds (how long to cache Redis lookups in memory)
    cacheTTL            300

    # Connection timeout in milliseconds
    redisTimeout        1000

    # Retry on connection failure
    retryOnFailure      1
    retryInterval       5

    # Default document root if Redis lookup fails
    defaultDocRoot      /var/www/html

    # Default PHP handler
    defaultPhpHandler   lsphp74
}
```

### Configuration Options Explained

| Option             | Default         | Description                                              |
|--------------------|-----------------|----------------------------------------------------------|
| `enabled`          | 0               | Master switch. `1` enables the module.                   |
| `redisHost`        | 127.0.0.1       | Redis server hostname or IP.                             |
| `redisPort`        | 6379            | Redis server port.                                       |
| `redisAuth`        | (none)          | Redis AUTH password. Omit if Redis has no password.      |
| `redisDb`          | 0               | Redis database number (0-15).                            |
| `redisKeyPrefix`   | vhost:          | Prefix for Redis keys. Domain is appended.               |
| `cacheTTL`         | 300             | Seconds to cache a resolved vhost in memory.             |
| `redisTimeout`     | 1000            | Connection timeout in milliseconds.                      |
| `retryOnFailure`   | 1               | Retry failed Redis connections. `1` = yes, `0` = no.     |
| `retryInterval`    | 5               | Seconds between retry attempts.                          |
| `defaultDocRoot`   | /var/www/html   | Fallback document root when Redis lookup fails.          |
| `defaultPhpHandler`| lsphp74         | Fallback PHP handler when not specified in Redis.        |

## Redis Data Format

Each domain is stored as a Redis key with a JSON value. The key format is:

```
{redisKeyPrefix}{domain_name}
```

For example, with the default prefix `vhost:`, the key for `example.com` would be `vhost:example.com`.

### Complete JSON Structure

```json
{
    "docRoot": "/home/customer1/example.com/public_html",
    "phpHandler": "lsphp81",
    "phpIniOverride": {
        "upload_max_filesize": "64M",
        "post_max_size": "64M",
        "memory_limit": "256M",
        "max_execution_time": "300"
    },
    "aliases": [
        "www.example.com",
        "shop.example.com"
    ],
    "ssl": {
        "certFile": "/etc/letsencrypt/live/example.com/fullchain.pem",
        "keyFile": "/etc/letsencrypt/live/example.com/privkey.pem"
    },
    "accessLog": "/home/customer1/logs/example.com.access.log",
    "errorLog": "/home/customer1/logs/example.com.error.log",
    "headers": {
        "X-Powered-By": "MyHosting",
        "Strict-Transport-Security": "max-age=31536000"
    },
    "rewrites": [
        "RewriteRule ^/old-page$ /new-page [R=301,L]"
    ],
    "enabled": true
}
```

### Field Reference

| Field             | Required | Description                                           |
|-------------------|----------|-------------------------------------------------------|
| `docRoot`         | Yes      | Absolute path to the document root directory.         |
| `phpHandler`      | No       | PHP handler name (e.g., `lsphp81`). Falls back to default. |
| `phpIniOverride`  | No       | Object of PHP ini directives to override per-vhost.   |
| `aliases`         | No       | Array of domain aliases that map to the same config.  |
| `ssl`             | No       | Object with `certFile` and `keyFile` paths.           |
| `accessLog`       | No       | Custom access log path for this vhost.                |
| `errorLog`        | No       | Custom error log path for this vhost.                 |
| `headers`         | No       | Object of custom response headers.                    |
| `rewrites`        | No       | Array of rewrite rules for this vhost.                |
| `enabled`         | No       | Boolean. Set to `false` to disable this vhost.        |

## Setting Up Redis for Your First Domain

### Step 1: Prepare the Document Root

```bash
mkdir -p /home/customer1/example.com/public_html
echo '<?php phpinfo(); ?>' > /home/customer1/example.com/public_html/index.php
chown -R nobody:nogroup /home/customer1/example.com/
```

### Step 2: Add the Domain to Redis

```bash
redis-cli SET "vhost:example.com" '{
    "docRoot": "/home/customer1/example.com/public_html",
    "phpHandler": "lsphp81",
    "aliases": ["www.example.com"],
    "enabled": true
}'
```

### Step 3: Verify the Record

```bash
redis-cli GET "vhost:example.com"
```

### Step 4: Test the Domain

```bash
curl -H "Host: example.com" http://your-server-ip/
```

You should see the PHP info page. No OLS restart was needed.

## Bulk Provisioning Domains

For hosting providers adding many domains at once, loop through a list and insert into Redis:

```bash
#!/bin/bash
# bulk_provision.sh -- Reads domains.csv (domain,username,php_version)
while IFS=',' read -r domain username php_version; do
    [[ "$domain" == "domain" ]] && continue
    home_dir="/home/${username}/${domain}/public_html"
    mkdir -p "$home_dir" "/home/${username}/logs"
    echo "<h1>Welcome to ${domain}</h1>" > "${home_dir}/index.html"
    chown -R nobody:nogroup "/home/${username}/"
    redis-cli SET "vhost:${domain}" "{\"docRoot\":\"${home_dir}\",\"phpHandler\":\"lsphp${php_version}\",\"aliases\":[\"www.${domain}\"],\"enabled\":true}"
    echo "Provisioned: ${domain}"
done < domains.csv
```

All domains become active immediately -- no server restart needed.

## Cache TTL Tuning

The `cacheTTL` setting controls how long OLS caches a Redis lookup in memory before querying Redis again.

| Scenario                     | Recommended TTL | Reason                                    |
|------------------------------|-----------------|-------------------------------------------|
| Stable hosting (few changes) | 3600 (1 hour)   | Reduces Redis queries significantly       |
| Active provisioning          | 60 (1 minute)   | New domains become active quickly         |
| Development/testing          | 10 (10 seconds) | Changes reflect almost immediately        |
| High-traffic production      | 300 (5 minutes) | Good balance of freshness and performance |

To force an immediate refresh without waiting for TTL, perform a graceful restart:

```bash
/usr/local/lsws/bin/lswsctrl restart
```

## Failover Behavior

### What Happens When Redis Is Down

If OLS cannot connect to Redis:

1. **Existing cached entries continue to work.** Domains that were recently resolved are served from the in-memory cache until their TTL expires.
2. **New/expired domains fall back to `defaultDocRoot`.** If a domain's cache entry has expired and Redis is unreachable, OLS uses the configured `defaultDocRoot`.
3. **If `retryOnFailure` is enabled**, OLS retries the Redis connection at `retryInterval` intervals.
4. **When Redis recovers**, OLS automatically reconnects on the next lookup attempt.

### Recommendations for High Availability

- Run Redis with persistence (`appendonly yes`) so data survives restarts.
- Use Redis Sentinel or Redis Cluster for automatic failover.
- Set `cacheTTL` high enough that brief Redis outages are invisible.
- Monitor Redis with `redis-cli info` and alerting on connection failures.

## Monitoring Redis VHost

### Check Module Status in OLS Logs

```bash
grep -i redisvhost /usr/local/lsws/logs/error.log
```

Look for:
- `[mod_redisvhost] Connected to Redis at 127.0.0.1:6379` -- successful connection
- `[mod_redisvhost] Redis lookup for "example.com": found` -- successful resolution
- `[mod_redisvhost] Redis lookup for "unknown.com": not found, using default` -- fallback
- `[mod_redisvhost] Redis connection failed, retrying in 5s` -- connection issue

### Debug with redis-cli

```bash
# Check if a domain exists
redis-cli EXISTS "vhost:example.com"
# Returns 1 (exists) or 0 (not found)

# View the config for a domain
redis-cli GET "vhost:example.com" | python3 -m json.tool

# List all configured domains
redis-cli KEYS "vhost:*"

# Count total domains
redis-cli KEYS "vhost:*" | wc -l

# Check Redis memory usage
redis-cli INFO memory | grep used_memory_human
```

## Performance Considerations

### Redis Query Latency

A local Redis query typically takes 0.1-0.5ms. With the in-memory cache, this only happens once per domain per TTL period. For 10,000 domains with a 5-minute TTL, that is roughly 33 Redis queries per second -- negligible load.

### Memory Usage

Each cached vhost entry in OLS memory is small (a few KB). Even with 50,000 domains, the in-memory cache uses under 100MB.

Redis memory usage depends on the size of your JSON configs. A typical entry is 500 bytes. For 10,000 domains, that is about 5MB in Redis.

### Scaling Tips

- Use Unix socket instead of TCP for local Redis: set `redisHost` to `/var/run/redis/redis.sock`.
- Use Redis pipelining for bulk operations (the bulk provisioning script above can be optimized with `redis-cli --pipe`).
- For 100,000+ domains, consider Redis Cluster to distribute the load.
- Set `cacheTTL` as high as your use case allows to minimize Redis queries.
