# Just-In-Time VHost Loading Guide

## The Problem: Slow Restarts with Many VHosts

When a web server starts (or restarts), it reads and parses every virtual host
configuration file before it begins accepting connections. For a shared hosting
server with hundreds or thousands of sites this means:

- **Startup takes minutes** -- each vhost config must be validated, SSL certs
  loaded, document roots checked, rewrite rules compiled, and PHP handlers
  initialized.
- **Graceful restarts become ungraceful** -- a config change on one site forces
  every site to reload.
- **Memory is wasted** -- vhosts that receive no traffic still consume RAM for
  their parsed config trees, SSL contexts, and handler pools.

On a shared hosting box with 500 vhosts, a cold start can take 30-60 seconds.
At 2,000 vhosts it can exceed several minutes. During that window the server
returns errors to every visitor.

## How JIT Loading Works

With JIT (Just-In-Time) VHost loading enabled, the server changes its startup
behavior:

1. **At startup** the server scans all `virtualhost` blocks but only records
   lightweight metadata: the vhost name, config file path, vhost root, and
   domain-to-vhost mapping. This is stored in a `JitVHostEntry` object that
   uses very little memory.

2. **No parsing happens yet** -- the full config (rewrite rules, SSL context,
   PHP handler pools, access controls) is not loaded.

3. **On first request** for a domain, the server looks up the domain in its
   vhost map. If the vhost has not been loaded yet, the JIT config loader
   parses the full config, creates the `HttpVHost` object, and installs it.

4. **Subsequent requests** hit the fully loaded vhost with zero overhead -- the
   JIT path is only taken once per vhost.

The result: a server with 2,000 vhosts starts in under 2 seconds instead of
several minutes. Only vhosts that actually receive traffic consume resources.

## Configuration

Enable JIT loading in your main server config:

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

jitVHost                  1
```

That single directive is all you need. There are no additional tuning knobs.

### Where to Set It

JIT loading is a server-level directive. It goes in the top-level
`httpd_config.conf`, not inside any `virtualhost` block.

```apacheconf
serverName                My Hosting Server
user                      nobody
group                     nogroup

# Enable JIT vhost loading
jitVHost                  1

# ... rest of server config
```

## Performance Impact

Here is a real-world comparison on a server with 500 virtual hosts:

| Metric                  | Without JIT | With JIT   |
|-------------------------|-------------|------------|
| Cold start time         | ~45 seconds | ~1.5 seconds |
| Memory at startup       | ~800 MB     | ~120 MB    |
| Memory after 50% active | ~800 MB     | ~480 MB    |
| First request to idle site | 0 ms overhead | ~50-100 ms (one-time) |
| Subsequent requests     | 0 ms overhead | 0 ms overhead |
| Graceful restart time   | ~45 seconds | ~1.5 seconds |

The one-time cost per vhost is typically 50-100ms for a standard config with
SSL. Visitors experience this as a barely noticeable delay on their very first
request. Every request after that is identical to a non-JIT server.

## When to Use It

**Use JIT loading when:**

- You have 100+ virtual hosts on a single server.
- You run shared hosting and many sites are low-traffic or dormant.
- You need fast graceful restarts for config changes.
- You want to minimize memory usage on servers with many sites.

**Skip JIT loading when:**

- You have fewer than 10 vhosts -- startup is already fast.
- Every single vhost receives constant traffic -- all will be loaded
  immediately anyway, so you just add a tiny delay to first requests.
- You need to validate all vhost configs at startup (JIT defers parse errors
  until first request).

## How It Handles Concurrent Requests

When multiple requests arrive simultaneously for an unloaded vhost, the JIT
loader uses internal locking to prevent duplicate loading:

1. The first request for `example.com` triggers the JIT load. The
   `JitVHostEntry` is marked as `m_bLoading = true`.
2. Any additional requests for `example.com` that arrive while loading is in
   progress will wait briefly for the load to complete rather than triggering a
   second load.
3. Once loading finishes, `m_bLoaded` is set to `true` and all waiting requests
   proceed with the fully initialized vhost.

This means you never get duplicate vhost objects or race conditions, even under
a sudden traffic spike to a previously idle site.

## Monitoring JIT Load Events

The server logs JIT load events to the error log. Watch for these messages:

```bash
# See all JIT load events
grep -i "jit\|just.in.time" /usr/local/lsws/logs/error.log

# Watch JIT loads in real time during a traffic spike
tail -f /usr/local/lsws/logs/error.log | grep -i "jit"
```

Typical log entries look like:

```
[INFO] JIT loading vhost [example.com], config: conf/vhosts/example.com/vhconf.conf
[INFO] JIT vhost [example.com] loaded successfully in 85ms
```

If a vhost config has errors, the JIT loader logs the error and returns a 503
to the requesting client:

```
[ERROR] JIT loading vhost [broken.com] failed: missing docRoot directive
```

### Checking Which VHosts Are Loaded

You can check the server's real-time status through the WebAdmin console at
`https://your-server:7080`. The virtual host list shows which vhosts are
currently active (loaded) versus dormant (waiting for first request).

From the command line, you can also check memory usage as a proxy:

```bash
# Check server memory footprint -- grows as more vhosts are JIT-loaded
ps aux | grep lshttpd | grep -v grep
```

## Limitations and Caveats

### Config Errors Are Deferred

Without JIT, a broken vhost config prevents the server from starting. You
discover the problem immediately. With JIT, the server starts fine but the
broken vhost fails when a visitor first requests it. This can make config
errors harder to catch.

**Mitigation:** After editing vhost configs, run a config test:

```bash
/usr/local/lsws/bin/lswsctrl configtest
```

This validates all configs without restarting the server.

### SSL Certificate Loading Is Deferred

SSL certificates for JIT vhosts are not loaded until first request. This means:

- Certificate expiry is not checked at startup for unloaded vhosts.
- The first HTTPS request to a JIT vhost takes slightly longer as the SSL
  context is initialized.

### Listeners Must Still Be Configured

JIT loading defers vhost config parsing, but the server still needs to know
which listeners (ports/IPs) to bind at startup. Listener configuration is
always loaded eagerly.

### Interaction with Redis Dynamic VHost

If you use both `jitVHost` and Redis Dynamic VHost (`redisVHostEnable`), the
Redis module takes priority for dynamic lookups. JIT loading applies only to
file-based vhosts defined in `httpd_config.conf`.

## Complete Example

A minimal production config for a shared hosting server with many sites:

```apacheconf
# /usr/local/lsws/conf/httpd_config.conf

serverName                Shared Hosting Server
user                      nobody
group                     nogroup

# Enterprise features
jitVHost                  1
sslAsyncHandshake         1
cpuAffinityMode           1

# Connections
maxConnections            10000
maxSSLConnections         10000
connTimeout               300
maxKeepAliveReq           10000
keepAliveTimeout          5

# Logging
errorlog /usr/local/lsws/logs/error.log {
    logLevel              WARN
    debugLevel            0
    rollingSize           10M
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

# These 500 vhosts will all be JIT-loaded on first request
virtualhost site001.com {
    vhRoot                /home/site001/site001.com
    configFile            conf/vhosts/site001.com/vhconf.conf
}

virtualhost site002.com {
    vhRoot                /home/site002/site002.com
    configFile            conf/vhosts/site002.com/vhconf.conf
}

# ... hundreds more vhosts ...
```
