# VHost Bandwidth Throttling Guide

## Why Bandwidth Throttling Matters

On a shared hosting server, one site can monopolize the network connection. A single client downloading a large file or a traffic spike to one site can starve every other site of bandwidth. VHost-level bandwidth throttling prevents this by capping how much bandwidth each virtual host can use.

Without throttling:
- Site A starts serving a viral video, consuming 900 Mbps of your 1 Gbps link
- Sites B through Z share the remaining 100 Mbps
- All other customers experience slow page loads

With throttling:
- Each site is capped at a fair share (e.g., 100 Mbps)
- Site A's video downloads are rate-limited
- Sites B through Z continue serving pages at normal speed

## How Aggregate Throttling Works

OpenLiteSpeed's `vhBandwidthLimit` is an **aggregate per-vhost** limit, not a per-client limit. This is an important distinction.

### Per-VHost (Aggregate)

`vhBandwidthLimit` caps the **total** bandwidth used by all connections to a virtual host combined.

```
vhBandwidthLimit = 10 MB/s

Client A downloading at 5 MB/s  --|
Client B downloading at 3 MB/s  --|-- Total: 10 MB/s (at limit)
Client C downloading at 2 MB/s  --|

Client D tries to download --> throttled, must wait
```

### Per-Client (Individual)

The separate per-client throttling (`outBandwidth` / `inBandwidth` in server config) caps each individual connection.

```
outBandwidth = 2 MB/s

Client A: capped at 2 MB/s
Client B: capped at 2 MB/s
Client C: capped at 2 MB/s
(each independently limited)
```

Both limits can be active at the same time. The stricter limit wins for any given connection.

## Configuration

### Basic Syntax

The `vhBandwidthLimit` is set inside a virtual host configuration block. The value is in **bytes per second**.

```xml
virtualHost example.com {
    vhRoot              /var/www/example.com
    configFile          conf/vhosts/example.com/vhconf.conf
    vhBandwidthLimit    10485760
}
```

You can also set it in the per-vhost config file (`vhconf.conf`):

```xml
vhBandwidthLimit        10485760
```

### Value Conversion Reference

The value is always in bytes per second. Here are common conversions:

| Desired Limit | Bytes/Second | Config Value |
|---------------|-------------|-------------|
| 1 Mbps | 131,072 | 131072 |
| 5 Mbps | 655,360 | 655360 |
| 10 Mbps | 1,310,720 | 1310720 |
| 50 Mbps | 6,553,600 | 6553600 |
| 100 Mbps | 13,107,200 | 13107200 |
| 500 Mbps | 65,536,000 | 65536000 |
| 1 Gbps | 134,217,728 | 134217728 |
| Unlimited | 0 | 0 |

**Conversion formula:** Mbps * 1,000,000 / 8 = bytes/sec (approximate). For exact values: Mbps * 1,048,576 / 8.

**Note:** A value of `0` means unlimited -- no throttling is applied.

## Configuration Examples

### Example 1: Budget Shared Hosting (1 Mbps per site)

For very cheap shared hosting plans where you need strict limits:

```xml
virtualHost budget-site.com {
    vhRoot              /var/www/budget-site.com
    configFile          conf/vhosts/budget-site.com/vhconf.conf
    vhBandwidthLimit    131072
}
```

At 1 Mbps, a 1 MB image takes about 8 seconds to download. This is very restrictive but protects the server.

### Example 2: Standard Shared Hosting (10 Mbps per site)

A reasonable default for shared hosting:

```xml
virtualHost standard-site.com {
    vhRoot              /var/www/standard-site.com
    configFile          conf/vhosts/standard-site.com/vhconf.conf
    vhBandwidthLimit    1310720
}
```

Enough for normal web browsing. A typical web page (2-3 MB) loads in under 3 seconds from the bandwidth perspective.

### Example 3: Premium Hosting (100 Mbps per site)

For premium plans or low-density shared hosting:

```xml
virtualHost premium-site.com {
    vhRoot              /var/www/premium-site.com
    configFile          conf/vhosts/premium-site.com/vhconf.conf
    vhBandwidthLimit    13107200
}
```

### Example 4: Unlimited (No Throttling)

For dedicated or VPS hosting where the customer owns the server:

```xml
virtualHost my-dedicated-site.com {
    vhRoot              /var/www/my-dedicated-site.com
    configFile          conf/vhosts/my-dedicated-site.com/vhconf.conf
    vhBandwidthLimit    0
}
```

## Interaction with Per-Client Throttling

OpenLiteSpeed has two layers of bandwidth control. They work together, and the stricter limit always wins.

### Server-Level Per-Client Settings

These are set in the main `httpd_config.conf`:

```xml
tuning {
    outBandwidth            2097152    # 2 MB/s per client outbound
    inBandwidth             1048576    # 1 MB/s per client inbound
}
```

### How They Combine

Consider this setup:

```
vhBandwidthLimit = 10 MB/s   (per-vhost aggregate)
outBandwidth     = 2 MB/s    (per-client)
```

Scenario with 3 clients downloading simultaneously:

```
Client A: wants 5 MB/s --> capped to 2 MB/s (per-client limit)
Client B: wants 5 MB/s --> capped to 2 MB/s (per-client limit)
Client C: wants 5 MB/s --> capped to 2 MB/s (per-client limit)
Total: 6 MB/s --> under 10 MB/s vhost limit, so all get their 2 MB/s
```

Scenario with 10 clients downloading simultaneously:

```
10 clients x 2 MB/s = 20 MB/s --> exceeds 10 MB/s vhost limit
OLS throttles all connections proportionally
Each client gets approximately 1 MB/s
```

### Recommended Combination

For shared hosting, use both limits together:

```xml
# Server level (httpd_config.conf)
tuning {
    outBandwidth            2097152    # 2 MB/s per client
    inBandwidth             524288     # 512 KB/s per client upload
}

# Per vhost
virtualHost site.com {
    vhBandwidthLimit        6553600    # 50 Mbps aggregate per vhost
}
```

This ensures:
- No single visitor hogs a site's bandwidth (per-client limit)
- No single site hogs the server's bandwidth (per-vhost limit)

## Monitoring Bandwidth Usage

### Real-Time Server Status

OpenLiteSpeed's real-time stats page shows per-vhost bandwidth usage:

```bash
# Access the admin console
# Navigate to: Server > Real-Time Stats
# Look for "BW In" and "BW Out" columns per vhost
```

The admin panel URL is typically: `https://your-server:7080/`

### Using System Tools

```bash
# Monitor network traffic per process
iftop -i eth0

# Check overall bandwidth usage
vnstat -l

# Watch OLS network activity specifically
nethogs -p eth0 | grep litespeed

# Check bandwidth usage over time
vnstat -d    # daily
vnstat -m    # monthly
```

### Checking if Throttling Is Active

When a vhost hits its bandwidth limit, you will see slower response times for that vhost while other vhosts remain fast. You can verify this by:

```bash
# Download a large file from the throttled vhost and measure speed
curl -o /dev/null -w "Speed: %{speed_download} bytes/sec\n" \
    https://example.com/large-file.zip

# Compare with expected limit
# If vhBandwidthLimit is 1310720 (10 Mbps), the download speed
# should not exceed approximately 1.3 MB/s
```

### Log-Based Monitoring

Check the access log for response sizes to estimate bandwidth:

```bash
# Sum up bytes sent in the last hour (assuming combined log format)
awk -v threshold=$(date -d '1 hour ago' '+%d/%b/%Y:%H') \
    '$4 ~ threshold {sum += $10} END {print sum/1048576 " MB"}' \
    /usr/local/lsws/logs/access.log
```

## Complete Shared Hosting Configuration

Here is a full example for a shared hosting server with tiered plans:

```xml
# httpd_config.conf - Server-level settings

serverProcessConfig {
    CPUAffinity         1
    children            4
}

tuning {
    outBandwidth            2097152     # 2 MB/s per client
    inBandwidth             524288      # 512 KB/s per client upload
    maxConnections          10000
    maxSSLConnections       5000
}

# --- Basic Plan: 10 Mbps, suitable for blogs and small sites ---

virtualHost basic-customer.com {
    vhRoot              /var/www/basic-customer.com
    configFile          conf/vhosts/basic-customer.com/vhconf.conf
    vhBandwidthLimit    1310720
    maxKeepAliveReq     50
}

# --- Standard Plan: 50 Mbps, suitable for business sites ---

virtualHost standard-customer.com {
    vhRoot              /var/www/standard-customer.com
    configFile          conf/vhosts/standard-customer.com/vhconf.conf
    vhBandwidthLimit    6553600
    maxKeepAliveReq     100
}

# --- Premium Plan: 200 Mbps, suitable for high-traffic sites ---

virtualHost premium-customer.com {
    vhRoot              /var/www/premium-customer.com
    configFile          conf/vhosts/premium-customer.com/vhconf.conf
    vhBandwidthLimit    26214400
    maxKeepAliveReq     200
}

# --- Reseller Plan: 500 Mbps ---

virtualHost reseller-customer.com {
    vhRoot              /var/www/reseller-customer.com
    configFile          conf/vhosts/reseller-customer.com/vhconf.conf
    vhBandwidthLimit    65536000
    maxKeepAliveReq     500
}
```

### Per-VHost Config (`vhconf.conf`) for a Standard Plan Site

```xml
docRoot                 $VH_ROOT/public_html/
index {
    indexFiles          index.php, index.html
}

# Bandwidth is controlled at the server config level via vhBandwidthLimit
# Additional per-vhost tuning:
scriptHandler {
    add lsapi:php    php
}

rewrite {
    enable              1
    rules               <<<END_rules
    RewriteEngine On
    RewriteCond %{REQUEST_FILENAME} !-f
    RewriteCond %{REQUEST_FILENAME} !-d
    RewriteRule ^(.*)$ /index.php/$1 [L]
    END_rules
}

# Cache settings to reduce bandwidth usage
context / {
    enableCache         1
    cacheExpire         86400
}
```

## Tips for Setting Bandwidth Limits

1. **Start generous, tighten later.** Begin with higher limits and lower them based on actual usage data. Overly tight limits frustrate customers.

2. **Account for SSL overhead.** TLS adds roughly 2-5% overhead. A 10 Mbps limit means about 9.5-9.8 Mbps of actual content delivery.

3. **Consider peak vs. sustained.** `vhBandwidthLimit` is a sustained rate, not a burst limit. Short bursts may briefly exceed the limit before throttling kicks in.

4. **Match your server's capacity.** If your server has a 1 Gbps connection and 20 vhosts, setting each to 200 Mbps means you are technically oversubscribing by 4x. This is fine -- not all sites peak simultaneously -- but do not oversubscribe by more than 10x.

5. **Combine with caching.** Cached responses are served from memory and are extremely fast. Bandwidth throttling applies to the final delivery to the client, so caching still helps reduce server-side resource usage even when bandwidth is the bottleneck.
