# Anti-DDoS Protection Guide

## How OLS Enterprise Anti-DDoS Works

Traditional web servers respond to connection abuse with a simple strategy:
too many connections from one IP means an immediate ban. This works but
creates false positives -- legitimate users behind corporate NATs, mobile
carriers, or CDNs share IP addresses and can get banned unfairly.

This fork implements an **escalation pipeline** inspired by LiteSpeed
Enterprise. Instead of jumping straight to a ban, the server progressively
increases its response:

```
Normal Traffic -> Soft Limit Hit -> reCAPTCHA Challenge -> Ban (if failed)
```

This gives real humans a chance to prove they are not bots before being
blocked.

## Connection Limits

The anti-DDoS system tracks per-client (per-IP) connections with four
thresholds:

| Parameter     | What It Does                                              |
|---------------|-----------------------------------------------------------|
| `softLimit`   | When exceeded, the server starts monitoring the client    |
| `hardLimit`   | When exceeded, the server takes action (captcha or ban)   |
| `gracePeriod` | Seconds to wait before enforcing the hard limit           |
| `banPeriod`   | How long (seconds) a banned client stays blocked          |

### How the Thresholds Interact

1. A client opens connections normally. Nothing happens.
2. The client exceeds `softLimit`. The server flags the IP and begins counting.
3. If the client stays above `softLimit` for longer than `gracePeriod` seconds
   and hits `hardLimit`, the server escalates.
4. With `antiDdosCaptcha` enabled, escalation means a reCAPTCHA challenge
   instead of an immediate ban.
5. If the client solves the captcha, they are whitelisted temporarily.
6. If the client fails or ignores the captcha, they are banned for `banPeriod`
   seconds.

## reCAPTCHA Escalation Flow

When `antiDdosCaptcha` is set to `1`, the full escalation flow is:

```
                    connections < softLimit
                           |
                      [Normal Traffic]
                           |
                  connections > softLimit
                           |
                    [Monitoring Phase]
                    (grace period countdown)
                           |
                  connections > hardLimit
                  AND grace period expired
                           |
                   [reCAPTCHA Challenge]
                   Server serves captcha page
                      /            \
                 [PASS]            [FAIL / Timeout]
                   |                      |
            [Whitelisted]            [Banned]
            Resume normal         Blocked for banPeriod
```

The reCAPTCHA page is served by an internal LSAPI handler
(`lsrecaptcha/_recaptcha`) and uses Google's reCAPTCHA v2 widget. You can also
provide a custom captcha page at
`$SERVER_ROOT/lsrecaptcha/_recaptcha_custom.shtml`.

## Configuration

### Basic Anti-DDoS Setup

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

# Enable reCAPTCHA escalation instead of immediate bans
antiDdosCaptcha           1

# Per-client connection limits
perClientConnLimit {
    softLimit             500
    hardLimit             1000
    gracePeriod           15
    banPeriod             300
}
```

### reCAPTCHA Keys

You need Google reCAPTCHA v2 keys. Get them at
https://www.google.com/recaptcha/admin.

```apacheconf
lsrecaptcha {
    enabled               1
    siteKey               YOUR_RECAPTCHA_V2_SITE_KEY
    secretKey             YOUR_RECAPTCHA_V2_SECRET_KEY
}
```

## Default Values and Recommended Production Values

| Parameter          | Stock OLS Default | This Fork Default | Recommended Production |
|--------------------|-------------------|-------------------|------------------------|
| `softLimit`        | 10,000            | INT_MAX (no limit)| 500                    |
| `hardLimit`        | 100               | 10,000            | 1,000                  |
| `gracePeriod`      | 15 seconds        | 15 seconds        | 15 seconds             |
| `banPeriod`        | 60 seconds        | 300 seconds       | 300-3600 seconds       |
| `antiDdosCaptcha`  | N/A               | 0 (off)           | 1 (on)                 |

### Why the Fork Changed the Defaults

Stock OLS has a `hardLimit` of 100 connections per client. This is far too low
for production shared hosting where:

- A single browser opens 6-10 connections per page load.
- HTTP/2 multiplexes but still holds connections open.
- Mobile apps and APIs can legitimately open many connections.
- Clients behind NAT/proxy share IP addresses.

This fork raises the hard limit to 10,000 to prevent false positives while
keeping the escalation pipeline available for real attacks.

## Combining with CloudFlare

If you use CloudFlare as a CDN/proxy, all traffic arrives from CloudFlare IP
addresses. Without adjustment, the anti-DDoS system would see CloudFlare as
one massive client and trigger limits immediately.

### Solution: Use Real IP Headers

```apacheconf
# Trust CloudFlare's Connecting-IP header
useIpInProxyHeader        1
```

This tells the server to use the `X-Forwarded-For` or `CF-Connecting-IP`
header for per-client tracking instead of the socket IP.

### CloudFlare + Anti-DDoS Best Practice

```apacheconf
# Trust CloudFlare IPs for real client detection
useIpInProxyHeader        1

# Higher limits since CloudFlare already filters some attacks
antiDdosCaptcha           1

perClientConnLimit {
    softLimit             300
    hardLimit             500
    gracePeriod           10
    banPeriod             600
}
```

CloudFlare's own DDoS protection handles volumetric attacks (Layer 3/4). The
server's anti-DDoS handles application-layer attacks (Layer 7) that CloudFlare
may pass through.

## Combining with ModSecurity WAF

ModSecurity and anti-DDoS serve different purposes and complement each other:

- **Anti-DDoS**: Limits based on connection count (volume-based).
- **ModSecurity**: Blocks based on request content (pattern-based).

Use both together for layered defense:

```apacheconf
# Layer 1: Anti-DDoS catches volume attacks
antiDdosCaptcha           1
perClientConnLimit {
    softLimit             500
    hardLimit             1000
    gracePeriod           15
    banPeriod             300
}

# Layer 2: ModSecurity catches malicious payloads
module mod_security {
    modsecurity           1
    modsecurity_rules_file /etc/modsecurity/owasp-crs/main.conf
    modsecAsync           1
}
```

A slow, sophisticated attacker sending carefully crafted SQL injection at low
volume will pass anti-DDoS checks but get caught by ModSecurity. A fast bot
flooding connections will get caught by anti-DDoS before ModSecurity even sees
the requests.

## Complete Production Anti-DDoS Config

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

serverName                Production Server
user                      nobody
group                     nogroup

antiDdosCaptcha           1

perClientConnLimit {
    softLimit             500
    hardLimit             1000
    gracePeriod           15
    banPeriod             600
}

lsrecaptcha {
    enabled               1
    siteKey               YOUR_RECAPTCHA_V2_SITE_KEY
    secretKey             YOUR_RECAPTCHA_V2_SECRET_KEY
}

maxConnections            10000
maxSSLConnections         10000
```

## Monitoring Attacks

### Checking Logs for DDoS Activity

```bash
# Look for connection limit triggers
grep -i "exceed\|ban\|captcha\|ddos\|limit" /usr/local/lsws/logs/error.log

# Watch in real time
tail -f /usr/local/lsws/logs/error.log | grep -i "ban\|captcha\|limit"

# Count banned IPs in the last hour
grep "ban" /usr/local/lsws/logs/error.log | \
    grep "$(date +%Y-%m-%d)" | wc -l
```

### Checking Per-Client Connection Counts

```bash
# See current connections per IP
ss -tn state established | awk '{print $5}' | \
    cut -d: -f1 | sort | uniq -c | sort -rn | head -20
```

This shows the top 20 IPs by active connection count. Compare these numbers
against your `softLimit` and `hardLimit` values.

## Emergency: Manually Banning IPs

If you need to block an IP immediately without waiting for the escalation
pipeline:

### Using iptables (Immediate Block)

```bash
# Ban a single IP
sudo iptables -A INPUT -s 203.0.113.50 -j DROP

# Ban a subnet
sudo iptables -A INPUT -s 203.0.113.0/24 -j DROP

# List current bans
sudo iptables -L INPUT -n --line-numbers

# Remove a ban (by line number from list above)
sudo iptables -D INPUT 3
```

### Using Server Access Control

Add to your server or vhost config for a persistent block:

```apacheconf
# In httpd_config.conf or vhconf.conf
accessControl {
    deny                  203.0.113.50
    deny                  198.51.100.0/24
}
```

Then restart:

```bash
sudo /usr/local/lsws/bin/lswsctrl restart
```

### Using fail2ban Integration

For automated banning, create a fail2ban jail that watches the OLS error log
for ban/limit messages and blocks offending IPs via iptables automatically.
