# CPU Affinity Configuration Guide

## What Is CPU Affinity?

CPU affinity controls which CPU cores a process is allowed to run on. By default, the operating system scheduler moves processes between cores freely. While this keeps all cores busy, it has a downside: every time a process moves to a different core, it loses the data it had cached in that core's L1/L2 cache. This is called a **cache miss**, and it forces the CPU to re-fetch data from slower main memory.

By pinning OpenLiteSpeed worker processes to specific CPU cores, you get **cache locality** -- the worker keeps running on the same core, so the data it needs stays warm in that core's cache. The result is lower latency and higher throughput, especially under heavy load.

### Quick Analogy

Think of CPU cores as desks in an office. Without affinity, a worker picks a random desk each morning and has to re-organize all their papers. With affinity, each worker has an assigned desk -- their tools are always right where they left them.

## Three Affinity Modes

OpenLiteSpeed provides three CPU affinity modes, configured in the server-level settings.

### Mode 0: Disabled (Default)

The OS scheduler decides which core each worker runs on. Processes may migrate between cores freely.

```xml
# httpd_config.conf
serverProcessConfig {
    CPUAffinity         0
}
```

**When to use:** Small servers with 1-2 cores, or when you are unsure which mode to pick. This is safe and works everywhere.

### Mode 1: Auto

OpenLiteSpeed automatically assigns each worker process to a different CPU core in round-robin order. If you have 4 cores and 4 workers, worker 0 goes to core 0, worker 1 to core 1, and so on.

```xml
# httpd_config.conf
serverProcessConfig {
    CPUAffinity         1
    children            4
}
```

**When to use:** Most production servers. It gives you the cache locality benefit without any manual tuning. Works well when the number of workers matches or is a multiple of the number of cores.

### Mode 2: Manual

You specify an explicit CPU affinity mask for each worker process. The mask is a bitmask where each bit represents a core.

```xml
# httpd_config.conf
serverProcessConfig {
    CPUAffinity         2
    CPUAffinityMask     3,12,48,192
}
```

The mask values are decimal numbers representing binary bitmasks:

| Decimal | Binary     | Cores Assigned |
|---------|-----------|----------------|
| 3       | 00000011  | Core 0, Core 1 |
| 12      | 00001100  | Core 2, Core 3 |
| 48      | 00110000  | Core 4, Core 5 |
| 192     | 11000000  | Core 6, Core 7 |

In this example, four workers each get a pair of cores to run on.

**When to use:** Advanced scenarios where you need to reserve cores for other services (database, caching) or isolate workloads precisely.

## Configuration Examples

### Example 1: 4-Core Server, General Purpose

Auto mode with one worker per core:

```xml
serverProcessConfig {
    CPUAffinity         1
    children            4
}
```

### Example 2: 8-Core Server, Reserving Cores for MySQL

Reserve cores 6-7 for MySQL, assign OLS workers to cores 0-5:

```xml
serverProcessConfig {
    CPUAffinity         2
    children            6
    CPUAffinityMask     1,2,4,8,16,32
}
```

Mask breakdown:
- Worker 0 -> Core 0 (mask 1 = binary 00000001)
- Worker 1 -> Core 1 (mask 2 = binary 00000010)
- Worker 2 -> Core 2 (mask 4 = binary 00000100)
- Worker 3 -> Core 3 (mask 8 = binary 00001000)
- Worker 4 -> Core 4 (mask 16 = binary 00010000)
- Worker 5 -> Core 5 (mask 32 = binary 00100000)

Then pin MySQL to cores 6-7 using systemd or taskset:

```bash
# Pin MySQL to cores 6 and 7
taskset -cp 192 $(pidof mysqld)
# 192 = binary 11000000 = cores 6,7
```

### Example 3: 16-Core Server, Two Workers Per Core Pair

Group workers onto pairs of cores for some scheduling flexibility while still limiting migration:

```xml
serverProcessConfig {
    CPUAffinity         2
    children            8
    CPUAffinityMask     3,3,12,12,48,48,192,192
}
```

Workers 0 and 1 share cores 0-1, workers 2 and 3 share cores 2-3, and so on.

### Example 4: Shared Hosting with Isolated VHosts

Use auto mode to spread workers across all available cores. Each vhost is handled by whichever worker picks up the connection, but the worker itself stays on its assigned core:

```xml
serverProcessConfig {
    CPUAffinity         1
    children            8
}
```

## Best Practices for Different Server Types

### Dedicated Web Server (OLS only)

Use **auto mode (1)**. Let OLS use all available cores. Set the number of workers equal to the number of CPU cores.

```xml
serverProcessConfig {
    CPUAffinity         1
    children            8    # Match your core count
}
```

### Web + Database on Same Server

Use **manual mode (2)**. Reserve 1-2 cores for the database, assign the rest to OLS.

```xml
# 8-core server: 6 cores for OLS, 2 for DB
serverProcessConfig {
    CPUAffinity         2
    children            6
    CPUAffinityMask     1,2,4,8,16,32
}
```

### Web + Redis + Database

Dedicate cores to each service:

```
Cores 0-3: OLS workers (4 workers)
Core 4:    Redis
Cores 5-7: MySQL/PostgreSQL
```

```xml
serverProcessConfig {
    CPUAffinity         2
    children            4
    CPUAffinityMask     1,2,4,8
}
```

### Small VPS (1-2 Cores)

Use **disabled (0)**. With so few cores, pinning provides no benefit and can actually hurt performance by preventing the OS from balancing load:

```xml
serverProcessConfig {
    CPUAffinity         0
}
```

## Checking Current Affinity

### See How Many Cores You Have

```bash
nproc
# Output: 8

# Or for more detail:
lscpu | grep "CPU(s):"
# CPU(s):              8
```

### Check OLS Worker Affinity

Find the OLS worker PIDs and check their affinity with `taskset`:

```bash
# Find OLS worker processes
ps aux | grep litespeed | grep -v grep

# Check affinity for a specific PID
taskset -cp <PID>
# Output: pid 12345's current affinity list: 0

# Check all OLS workers at once
for pid in $(pgrep litespeed); do
    echo -n "PID $pid: "
    taskset -cp $pid
done
```

Example output with auto mode on a 4-core server:

```
PID 1001: pid 1001's current affinity list: 0
PID 1002: pid 1002's current affinity list: 1
PID 1003: pid 1003's current affinity list: 2
PID 1004: pid 1004's current affinity list: 3
```

### Verify Cores Are Being Used Efficiently

```bash
# Watch per-core CPU usage in real time
mpstat -P ALL 1

# Or use htop with per-core display
htop
```

## When NOT to Use Manual Mode

Manual affinity mode (2) is powerful but can backfire in several situations:

1. **You have fewer workers than cores.** Some cores sit completely idle while pinned workers are overloaded. Use auto mode instead -- it handles this correctly.

2. **You are running in a VM or container with dynamic CPU allocation.** If the hypervisor changes how many cores are available, your hardcoded masks may reference cores that no longer exist, causing errors or suboptimal distribution.

3. **You do not understand bitmasks.** A wrong mask can pin multiple workers to the same core while leaving other cores empty. If in doubt, use auto mode.

4. **Your workload is bursty and unpredictable.** If traffic is very uneven, the OS scheduler can actually do a better job of load balancing than static pinning. Consider disabled mode or auto mode.

5. **You are running on a NUMA system and do not account for NUMA topology.** Pinning a worker to a core on one NUMA node while its memory is on another node creates cross-node memory access, which is slower than no pinning at all.

## Quick Reference

| Mode | Value | Best For | Complexity |
|------|-------|----------|------------|
| Disabled | 0 | Small servers, VMs, unsure | None |
| Auto | 1 | Most production servers | None |
| Manual | 2 | Multi-service servers, NUMA-aware tuning | High |

## Verifying Your Configuration

After changing the affinity setting, restart OLS and confirm:

```bash
# Restart OLS
systemctl restart lsws

# Wait a moment, then verify
for pid in $(pgrep litespeed); do
    echo -n "PID $pid: "
    taskset -cp $pid
done
```

If the output shows each worker pinned to a different core (auto mode) or your specified cores (manual mode), the configuration is working correctly.
