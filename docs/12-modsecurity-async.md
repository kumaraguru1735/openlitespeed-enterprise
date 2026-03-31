# Async ModSecurity WAF Guide

## What ModSecurity Is

ModSecurity is a Web Application Firewall (WAF) that inspects every HTTP
request and response against a set of rules. It can detect and block:

- SQL injection attacks (`' OR 1=1 --`)
- Cross-site scripting (XSS) (`<script>alert('xss')</script>`)
- Remote file inclusion
- Path traversal (`../../etc/passwd`)
- Malicious file uploads
- Protocol violations
- Scanner and bot detection

Think of it as a security guard that reads every letter (HTTP request) coming
into your building (server) and checks it against a list of known threats
before letting it through.

## How Async Mode Improves Performance

### The Problem with Synchronous ModSecurity

In standard (synchronous) mode, ModSecurity rule evaluation happens on the
main event loop thread. The server processes requests one at a time through
the WAF:

```
Request -> [Event Loop: Parse -> WAF Check -> Backend -> Response]
                                  ^^^^^^^^
                              Blocks event loop
                              (1-5ms per request)
```

With a large rule set (OWASP CRS has 200+ rules), each request can take 1-5ms
for WAF evaluation. Under high load this becomes a bottleneck -- the event
loop cannot accept new connections while evaluating WAF rules.

### How Async Mode Works

This fork offloads WAF rule evaluation to a dedicated thread pool:

```
Request -> [Event Loop: Parse -> Dispatch to WAF Thread Pool]
                                         |
                              [WAF Thread 1: Evaluate rules]
                              [WAF Thread 2: Evaluate rules]
                              [WAF Thread 3: Evaluate rules]
                                         |
           [Event Loop: Receive result <- Allow/Block decision]
```

The event loop hands off the request body and headers to a worker thread,
then continues accepting new connections. When the WAF thread finishes, it
signals the event loop with the result. This means:

- The event loop is never blocked by WAF processing.
- Multiple requests can be evaluated in parallel.
- Under high load, throughput improves dramatically.

### Performance Comparison

| Metric                    | Sync Mode       | Async Mode      |
|---------------------------|-----------------|-----------------|
| Requests/sec (no WAF)     | 15,000          | 15,000          |
| Requests/sec (OWASP CRS)  | 3,000-5,000     | 10,000-13,000   |
| P99 latency (OWASP CRS)   | 15-25ms         | 3-8ms           |
| Event loop blocking        | Yes (1-5ms/req) | No              |
| CPU usage pattern          | Single core hot | Spread across cores |

## Installing ModSecurity and OWASP CRS Rules

### Step 1: Install libmodsecurity

```bash
# Ubuntu/Debian
sudo apt-get install -y libmodsecurity-dev libmodsecurity3

# CentOS/RHEL/AlmaLinux
sudo dnf install -y libmodsecurity-devel libmodsecurity
```

### Step 2: Build OLS with ModSecurity Support

```bash
cd /root/OLS
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DMOD_SECURITY=ON
make -j$(nproc)
sudo make install
```

### Step 3: Install OWASP Core Rule Set (CRS)

```bash
# Create directory
sudo mkdir -p /etc/modsecurity

# Download OWASP CRS v4
cd /etc/modsecurity
sudo git clone https://github.com/coreruleset/coreruleset.git owasp-crs
cd owasp-crs

# Set up the config
sudo cp crs-setup.conf.example crs-setup.conf
```

### Step 4: Create Config Files

Create `/etc/modsecurity/modsecurity.conf` with `SecRuleEngine On`,
request body limits, audit logging to `/var/log/modsecurity/audit.log`,
and `SecDebugLogLevel 0` for production.

Then create `/etc/modsecurity/main.conf` that includes:

```
Include /etc/modsecurity/modsecurity.conf
Include /etc/modsecurity/owasp-crs/crs-setup.conf
Include /etc/modsecurity/owasp-crs/rules/*.conf
```

### Step 5: Create Required Directories

```bash
sudo mkdir -p /tmp/modsecurity /var/log/modsecurity
sudo chown nobody:nogroup /tmp/modsecurity /var/log/modsecurity
```

## Configuration in OLS

Add the ModSecurity module block to your server config:

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

module mod_security {
    modsecurity               1
    modsecurity_rules_file    /etc/modsecurity/main.conf
    modsecAsync               1
}
```

### Parameter Reference

| Parameter                  | Values  | Description                          |
|----------------------------|---------|--------------------------------------|
| `modsecurity`              | 0 or 1  | Enable/disable ModSecurity           |
| `modsecurity_rules_file`   | path    | Path to rules configuration file     |
| `modsecAsync`              | 0 or 1  | Enable async thread pool processing  |

## OWASP Core Rule Set Setup Step by Step

The OWASP CRS ships with a "paranoia level" system. Higher levels catch more
attacks but generate more false positives:

| Paranoia Level | Description                  | False Positive Risk |
|----------------|------------------------------|---------------------|
| 1 (default)    | Basic protections            | Very low            |
| 2              | Additional rules             | Low                 |
| 3              | Stricter matching            | Medium              |
| 4              | Maximum security             | High                |

### Configuring Paranoia Level

Edit `/etc/modsecurity/owasp-crs/crs-setup.conf`:

```apacheconf
# Start with level 1, increase after testing
SecAction "id:900000, phase:1, pass, t:none, \
    setvar:tx.paranoia_level=1"
```

### Recommended CRS Tuning

```apacheconf
# /etc/modsecurity/owasp-crs/crs-setup.conf

# Paranoia level (start with 1)
SecAction "id:900000, phase:1, pass, t:none, \
    setvar:tx.paranoia_level=1"

# Anomaly scoring threshold (lower = stricter)
SecAction "id:900110, phase:1, pass, t:none, \
    setvar:tx.inbound_anomaly_score_threshold=5, \
    setvar:tx.outbound_anomaly_score_threshold=4"

# Allowed HTTP methods
SecAction "id:900200, phase:1, pass, t:none, \
    setvar:'tx.allowed_methods=GET HEAD POST OPTIONS PUT DELETE'"

# Allowed content types
SecAction "id:900220, phase:1, pass, t:none, \
    setvar:'tx.allowed_request_content_type=|application/x-www-form-urlencoded| \
    |multipart/form-data| |text/xml| |application/xml| |application/json|'"
```

## Custom Rules Examples

### Block SQL Injection Patterns

```apacheconf
# /etc/modsecurity/custom-rules.conf

# Block common SQL injection in query strings
SecRule ARGS "@rx (?i)(\bunion\b.*\bselect\b|\bselect\b.*\bfrom\b.*\bwhere\b)" \
    "id:100001, phase:2, deny, status:403, \
    log, msg:'SQL Injection attempt detected', \
    tag:'attack-sqli'"

# Block SQL injection in cookies
SecRule REQUEST_COOKIES "@rx (?i)(\'|\"|;|--|\b(or|and)\b\s+\d+\s*=\s*\d+)" \
    "id:100002, phase:2, deny, status:403, \
    log, msg:'SQL Injection in cookie', \
    tag:'attack-sqli'"
```

### Block XSS Attacks

```apacheconf
# Block script tags in all parameters
SecRule ARGS "@rx (?i)<script[^>]*>.*?</script>" \
    "id:100010, phase:2, deny, status:403, \
    log, msg:'XSS script tag detected', \
    tag:'attack-xss'"

# Block event handler attributes
SecRule ARGS "@rx (?i)\bon\w+\s*=\s*['\"]" \
    "id:100011, phase:2, deny, status:403, \
    log, msg:'XSS event handler detected', \
    tag:'attack-xss'"
```

### Block Malicious File Uploads

```apacheconf
# Block PHP files in uploads
SecRule FILES_NAMES "@rx \.(php|phtml|phar|php[345]|phps)$" \
    "id:100020, phase:2, deny, status:403, \
    log, msg:'PHP file upload blocked', \
    tag:'attack-upload'"

# Block executable files
SecRule FILES_NAMES "@rx \.(exe|bat|cmd|sh|bash|cgi|pl|py)$" \
    "id:100021, phase:2, deny, status:403, \
    log, msg:'Executable file upload blocked', \
    tag:'attack-upload'"
```

Include custom rules in your main config:

```bash
# Add to /etc/modsecurity/main.conf
echo 'Include /etc/modsecurity/custom-rules.conf' | \
    sudo tee -a /etc/modsecurity/main.conf
```

## Async vs Sync Mode Comparison

| Aspect                  | Sync (`modsecAsync 0`)   | Async (`modsecAsync 1`)  |
|-------------------------|--------------------------|--------------------------|
| Processing thread       | Event loop (main)        | Dedicated WAF thread pool|
| Event loop blocking     | Yes                      | No                       |
| Max throughput           | Limited by WAF speed     | Limited by CPU cores     |
| Latency under load      | Increases linearly       | Stays flat               |
| Memory usage            | Lower (no thread pool)   | Slightly higher          |
| Debugging ease           | Simpler stack traces     | Thread context switching |
| Recommended for          | Development, testing     | Production               |

**Rule of thumb:** Always use `modsecAsync 1` in production. Only use sync
mode when debugging WAF issues where you need simpler log output.

## Troubleshooting False Positives

False positives are legitimate requests that ModSecurity blocks by mistake.
Common examples:

- A blog post containing SQL keywords (`SELECT`, `UNION`).
- A form submission with HTML content (WYSIWYG editors).
- API calls with JSON payloads that match attack patterns.

### Finding False Positives

```bash
# Check the audit log for recent blocks
tail -100 /var/log/modsecurity/audit.log

# Find which rule IDs are triggering
grep "id \"" /var/log/modsecurity/audit.log | \
    sort | uniq -c | sort -rn | head -20

# Check OLS error log for ModSecurity messages
grep -i "modsec\|mod_security" /usr/local/lsws/logs/error.log | tail -20
```

### Whitelisting a Rule for a Specific URL

```apacheconf
# Disable rule 941100 (XSS) for WordPress post editor
SecRule REQUEST_URI "@beginsWith /wp-admin/post.php" \
    "id:100100, phase:1, pass, nolog, \
    ctl:ruleRemoveById=941100"
```

### Whitelisting a Rule for a Specific Parameter

```apacheconf
# Allow HTML in the 'content' parameter for CMS editors
SecRuleUpdateTargetById 941100 "!ARGS:content"
SecRuleUpdateTargetById 941110 "!ARGS:content"
SecRuleUpdateTargetById 941120 "!ARGS:content"
```

### Disabling a Rule Globally

Only do this if you understand the security implications:

```apacheconf
# Disable a specific rule entirely
SecRuleRemoveById 941100

# Disable a range of rules
SecRuleRemoveById 941100-941999
```

## Complete Production WAF Config

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

serverName                Production WAF Server
user                      nobody
group                     nogroup

sslAsyncHandshake         1
jitVHost                  1
antiDdosCaptcha           1

maxConnections            10000
maxSSLConnections         10000

# Async ModSecurity WAF
module mod_security {
    modsecurity               1
    modsecurity_rules_file    /etc/modsecurity/main.conf
    modsecAsync               1
}

errorlog /usr/local/lsws/logs/error.log {
    logLevel              WARN
    rollingSize           10M
}

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
```

```apacheconf
# /etc/modsecurity/main.conf

# Base ModSecurity config
Include /etc/modsecurity/modsecurity.conf

# OWASP CRS setup and rules
Include /etc/modsecurity/owasp-crs/crs-setup.conf
Include /etc/modsecurity/owasp-crs/rules/*.conf

# Custom rules (loaded after CRS so they can override)
Include /etc/modsecurity/custom-rules.conf
```

After setting up, restart the server and verify:

```bash
# Restart
sudo /usr/local/lsws/bin/lswsctrl restart

# Verify ModSecurity is loaded
grep -i "modsec" /usr/local/lsws/logs/error.log | tail -5

# Test with a known attack pattern (should return 403)
curl -I "http://localhost/?id=1' OR 1=1--"

# Test normal request (should return 200)
curl -I "http://localhost/"
```
