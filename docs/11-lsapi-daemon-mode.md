# LSAPI Daemon/ProcessGroup Mode Guide

## The Problem: PHP Process Startup Overhead

In the default LSAPI configuration, the web server spawns a new PHP process
(or reuses one from a small pool) for each request. While LSAPI is already
faster than FastCGI or PHP-FPM, there is still overhead:

- **Process creation**: forking and exec'ing a new `lsphp` binary takes 10-50ms.
- **PHP initialization**: loading php.ini, extensions, and opcache takes
  another 20-100ms.
- **Shared hosting multiplier**: on a server with 200 users, each user's PHP
  processes start and stop independently, creating constant churn.

For a WordPress page that takes 200ms to render, spending 50-100ms just
starting the PHP process means 25-50% overhead on every cold request.

## What Daemon Mode Does

Daemon mode changes the LSAPI process lifecycle fundamentally:

### Without Daemon Mode (Default)

```
Request arrives -> Server forks lsphp -> PHP initializes -> Process request
                                                         -> lsphp exits
Request arrives -> Server forks lsphp -> PHP initializes -> Process request
                                                         -> lsphp exits
```

Every request (or every Nth request) pays the startup cost.

### With Daemon Mode

```
Server starts -> Daemon lsphp process starts (persistent parent)
                    |
Request arrives -> Daemon forks child -> Process request -> Child handles
                                                            next request
Request arrives -> Daemon forks child -> Process request -> Child handles
                                                            next request
                    |
              (Daemon stays alive, children are recycled)
```

The daemon (parent) process stays running permanently. It has already loaded
PHP, all extensions, and opcache. When a request arrives, it forks a child.
Because fork() copies the already-initialized PHP state, the child starts
nearly instantly (under 1ms vs 50-100ms).

## Performance Improvement

| Scenario                    | Without Daemon | With Daemon |
|-----------------------------|----------------|-------------|
| First request (cold start)  | 150-200ms      | 50-80ms     |
| Subsequent requests (warm)  | 5-20ms         | 2-5ms       |
| PHP process startup time    | 50-100ms       | < 1ms (fork)|
| Memory per idle user        | 0 (no process) | ~20MB (daemon) |
| Requests/sec (WordPress)    | ~50-80         | ~120-200    |

The tradeoff is memory: daemon mode keeps a persistent process per user. On a
server with 200 hosting accounts, that means ~4GB of base memory for daemon
processes. But the performance gain is substantial.

## Configuration

### Full extprocessor Block

```apacheconf
# In vhconf.conf or httpd_config.conf

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

    # --- Daemon Mode Settings ---
    lsapiDaemonMode       1
    lsapiMaxChildren      5
    lsapiMaxIdleTime      300
    lsapiMaxReqs          10000
}

scripthandler {
    add                   lsapi:lsphp php
}
```

### Parameter Reference

| Parameter          | Default | Range      | Description                          |
|--------------------|---------|------------|--------------------------------------|
| `lsapiDaemonMode`  | `0`     | 0, 1, 2   | 0=off, 1=daemon, 2=processgroup     |
| `lsapiMaxChildren` | `5`     | 1-100     | Max child processes per daemon       |
| `lsapiMaxIdleTime` | `300`   | 10-86400  | Seconds before idle daemon exits     |
| `lsapiMaxReqs`     | `10000` | 1-1000000 | Requests before recycling a child    |

## lsapiDaemonMode Values

### Mode 0: Off (Default)

Standard LSAPI behavior. The server manages PHP process lifecycle directly.
Each `lsphp` process handles requests and eventually exits.

Use this for: development, low-traffic single sites, debugging PHP issues.

### Mode 1: Daemon Mode

A persistent parent `lsphp` process stays running. It forks children to handle
requests. The parent never exits (until the server stops or idle timeout is
reached).

Use this for: production hosting, WordPress sites, any PHP application where
performance matters.

### Mode 2: ProcessGroup Mode

Similar to daemon mode but uses process groups for better resource isolation.
Each user gets their own process group. The server can track and limit
resources per group.

Use this for: shared hosting where you need per-user resource accounting and
stricter isolation between accounts.

## Tuning Max Children Per User

The `lsapiMaxChildren` setting controls how many concurrent PHP requests a
single user can process. Setting this correctly is critical:

**Too low** (1-2): PHP requests queue up. Slow pages block faster ones. AJAX
calls stall while a long-running script runs.

**Too high** (50+): One user's traffic spike can starve other users of CPU and
memory.

### Guidelines by Server Type

| Server Type             | Users | lsapiMaxChildren | Reasoning                |
|-------------------------|-------|-------------------|--------------------------|
| Single high-traffic site| 1     | 20-50             | Use all available cores  |
| Small shared hosting    | 10-20 | 5-10              | Fair sharing             |
| Large shared hosting    | 100+  | 3-5               | Prevent resource hogging |
| Reseller with VPS-like  | 5-10  | 10-20             | More resources per user  |

### Formula

A reasonable starting point:

```
lsapiMaxChildren = (Total CPU Cores * 2) / Number of Active Users
```

For a 16-core server with 50 active users: `(16 * 2) / 50 = ~1` (use
minimum of 3).

## Idle Timeout and Request Recycling

### lsapiMaxIdleTime

How long (in seconds) the daemon parent process stays alive with no requests.
After this period, the daemon exits and will be respawned on the next request.

- **300 seconds (default)**: Good for most cases. Saves memory for inactive
  sites while avoiding frequent restarts.
- **60 seconds**: Aggressive recycling for memory-constrained servers.
- **3600 seconds**: Keep daemons alive longer for sites with sporadic but
  regular traffic.

### lsapiMaxReqs

How many requests a child process handles before being recycled (killed and
replaced). This prevents memory leaks from accumulating:

- **10000 (default)**: Handles 10,000 requests before recycling. Good for most
  PHP applications.
- **1000**: More aggressive recycling for PHP applications with known memory
  leaks.
- **100000**: Less recycling for well-behaved applications where you want to
  maximize the benefit of opcache warmth.

## Monitoring Daemon Processes

### Checking Running Daemons

```bash
# List all lsphp daemon processes
ps aux | grep lsphp | grep -v grep

# Count daemon parent processes (one per active user in daemon mode)
ps aux | grep lsphp | grep -v grep | grep -c "ppid=1\|lshttpd"

# Show process tree (parent + children)
pstree -p $(pgrep lshttpd | head -1) | grep lsphp
```

### Checking Logs

```bash
# Look for daemon start/stop events
grep -i "daemon\|lsapi" /usr/local/lsws/logs/error.log | tail -20

# Check for PHP crashes or timeouts
grep -i "lsphp\|lsapi.*error\|lsapi.*timeout" /usr/local/lsws/logs/error.log
```

### Monitoring Memory Usage

```bash
# Memory used by all lsphp processes
ps aux | grep lsphp | grep -v grep | \
    awk '{sum += $6} END {printf "Total: %.1f MB\n", sum/1024}'

# Per-process memory breakdown
ps -eo pid,user,rss,comm | grep lsphp | \
    awk '{printf "PID: %s  User: %-12s  RSS: %.1f MB\n", $1, $2, $3/1024}'
```

## suEXEC with Daemon Mode

On shared hosting, each user's PHP processes must run as that user (not as
`nobody` or `www-data`). Daemon mode works with suEXEC to achieve this:

```apacheconf
extprocessor lsphp {
    type                  lsapi
    address               uds://tmp/lshttpd/lsphp_$VH_USER.sock
    autoStart             2
    path                  /usr/local/lsws/lsphp81/bin/lsphp

    # suEXEC runs the daemon as the vhost owner
    # The daemon and all its children run as that user
    lsapiDaemonMode       1
    lsapiMaxChildren      5
    lsapiMaxIdleTime      300
    lsapiMaxReqs          10000
}
```

With `autoStart 2` (detached mode), the server launches `lsphp` via suEXEC.
The daemon process runs as the vhost owner. All forked children inherit that
UID. This gives you:

- Per-user PHP process isolation.
- Per-user resource limits via `memSoftLimit` / `memHardLimit`.
- Per-user opcache (each user has their own warmed cache).

## Complete Shared Hosting PHP Config

```apacheconf
# /usr/local/lsws/conf/vhosts/user1-example.com/vhconf.conf

docRoot                   /home/user1/example.com/public_html

extprocessor lsphp {
    type                  lsapi
    address               uds://tmp/lshttpd/lsphp_user1.sock
    maxConns              5
    env                   PHP_LSAPI_CHILDREN=5
    env                   LSAPI_PGRP_MAX_IDLE=300
    initTimeout           60
    retryTimeout          0
    persistConn           1
    respBuffer            0
    autoStart             2
    path                  /usr/local/lsws/lsphp81/bin/lsphp
    backlog               50
    instances             1
    memSoftLimit          512M
    memHardLimit          1024M
    procSoftLimit         200
    procHardLimit         250

    lsapiDaemonMode       1
    lsapiMaxChildren      3
    lsapiMaxIdleTime      300
    lsapiMaxReqs          10000
}

scripthandler {
    add                   lsapi:lsphp php
}

rewrite {
    enable                1
    autoLoadHtaccess      1
}
```

## Troubleshooting PHP Performance

### PHP Processes Not Starting

```bash
# Check if lsphp binary exists and is executable
ls -la /usr/local/lsws/lsphp81/bin/lsphp

# Try running it manually
/usr/local/lsws/lsphp81/bin/lsphp -v

# Check error log for startup failures
grep "lsphp\|lsapi\|spawn" /usr/local/lsws/logs/error.log | tail -20
```

### High Memory Usage

If daemon processes consume too much memory:

1. Lower `lsapiMaxChildren` (fewer children per user).
2. Lower `lsapiMaxReqs` (recycle children more often to free leaked memory).
3. Lower `lsapiMaxIdleTime` (let idle daemons exit sooner).
4. Set tighter `memSoftLimit` / `memHardLimit` in the extprocessor block.

### Requests Timing Out

If PHP requests time out:

1. Check `initTimeout` -- increase if PHP takes long to start.
2. Check `lsapiMaxChildren` -- too few means requests queue up.
3. Check if the application itself is slow (database queries, external APIs).
4. Review `maxConns` -- if set too low, the server cannot send requests to PHP.

### Daemon Not Recycling

If a daemon process seems stuck:

```bash
# Find the daemon PID
ps aux | grep lsphp | grep -v grep

# Send SIGTERM to gracefully stop it (server will respawn on next request)
kill -TERM <daemon_pid>

# If unresponsive, force kill
kill -9 <daemon_pid>
```
