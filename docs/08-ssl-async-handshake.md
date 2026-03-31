# Async SSL Handshake Guide

## The Problem: Why Synchronous SSL Handshakes Are Slow

Every HTTPS connection starts with a TLS handshake. During this handshake, the server must perform cryptographic operations -- key exchange, certificate verification, and session setup. These operations are CPU-intensive.

In a **synchronous** model, the server thread that handles the connection is blocked during the entire handshake. While it waits for the cryptographic math to complete, it cannot process other requests. On a busy server handling thousands of concurrent HTTPS connections, this creates a bottleneck:

```
Synchronous model:

Thread 1: [--- TLS handshake (2-5ms) ---][--- serve request ---]
Thread 2:                                 [--- TLS handshake (2-5ms) ---][--- serve ---]
Thread 3:                                                                 [--- TLS hand...

--> Threads pile up waiting for crypto operations
--> New connections queue up
--> Latency increases under load
```

A single TLS handshake takes 2-5ms on modern hardware. That does not sound like much, but at 10,000 new connections per second, those milliseconds add up and exhaust the thread pool.

## How Async Handshake Works

The async SSL handshake feature in OpenLiteSpeed offloads the CPU-intensive cryptographic operations to a dedicated thread pool. The main event loop thread initiates the handshake and immediately moves on to handle other connections. When the crypto thread pool completes the handshake, it signals the event loop to resume the connection.

```
Async model:

Event loop:  [start TLS 1][start TLS 2][start TLS 3][serve req A][serve req B]...
                  |              |              |
Crypto pool: [--- compute ---]  |              |
             [--- compute ------]              |
             [--- compute --------]
                  |
Event loop:  ...[resume TLS 1][resume TLS 2][resume TLS 3]...

--> Event loop is never blocked
--> New connections are accepted immediately
--> Crypto work happens in parallel
```

The result is that the event loop stays responsive even under heavy TLS handshake load. Connection latency decreases because new connections are not waiting in a queue behind ongoing handshakes.

## When to Enable It

Enable async SSL handshake when:

- Your server handles **more than 500 new HTTPS connections per second**.
- You see **high latency on initial page loads** but fast subsequent requests (indicating handshake delay, not application slowness).
- Your CPU usage is **dominated by SSL/TLS operations** (check with `perf top` -- look for `libssl` or `libcrypto` functions).
- You are running a **high-traffic website, API gateway, or CDN edge node**.

You may not need it when:

- Your traffic is low (under 100 new connections per second).
- Most connections use HTTP/2 or HTTP/3 with connection reuse (fewer new handshakes).
- TLS session resumption handles most returning visitors.

## Configuration

Enabling async SSL handshake is a single configuration directive. Add it to your OLS server-level configuration:

```
sslAsyncHandshake    1
```

That is it. Set to `1` to enable, `0` to disable.

### Where to Add It

In `httpd_config.conf`, add it under the server-level tuning section:

```
tuning {
    sslAsyncHandshake    1
}
```

Or via the OLS admin panel:

1. Go to **Server Configuration > Tuning**.
2. Find **SSL Async Handshake**.
3. Set to **Yes**.
4. Perform a graceful restart.

## Performance Impact

The performance improvement depends on your traffic pattern and hardware. Here is what to expect:

### Low Traffic (< 100 new TLS connections/sec)

Minimal difference. The synchronous model handles this load fine. Enabling async adds a small amount of overhead from thread pool management.

### Medium Traffic (100-1,000 new TLS connections/sec)

Noticeable improvement in tail latency (p99). The average response time may drop by 10-30% for the initial connection. Subsequent requests on the same connection are unaffected (they reuse the established TLS session).

### High Traffic (1,000+ new TLS connections/sec)

Significant improvement. Expect:
- 30-50% reduction in TLS handshake latency.
- Higher maximum connections per second before latency degrades.
- More stable response times under load spikes.
- Better CPU utilization (crypto work is parallelized across cores).

### How to Benchmark

Test with and without async handshake using `wrk` or `h2load`:

```bash
# Install h2load (part of nghttp2)
apt install nghttp2-client -y

# Benchmark without async (baseline)
h2load -n 10000 -c 100 -t 4 https://your-server.com/

# Enable async handshake, restart OLS, then run the same test
h2load -n 10000 -c 100 -t 4 https://your-server.com/
```

Compare the "time for request" values, especially the p99 and max columns.

## Thread Pool Sizing

The async handshake uses OLS's internal thread pool. The pool size is managed automatically by default, but you can tune it for your workload:

```
tuning {
    sslAsyncHandshake    1
    sslCryptoThreads     4
}
```

### Guidelines for Thread Count

| CPU Cores | Recommended Threads | Reasoning                                    |
|-----------|---------------------|----------------------------------------------|
| 1-2       | 1-2                 | Limited cores, keep overhead low              |
| 4         | 2-3                 | Leave cores for the event loop and PHP        |
| 8         | 4-6                 | Good parallelism without starving other work  |
| 16+       | 6-8                 | Diminishing returns beyond 8 threads          |

Do not set the thread count higher than your CPU core count minus 2. The event loop and PHP processes need cores too.

## Monitoring SSL Handshake Performance

### Check Handshake Latency

Use `openssl s_client` to measure a single handshake:

```bash
time openssl s_client -connect your-server.com:443 -servername your-server.com </dev/null 2>/dev/null
```

Run this multiple times and compare the elapsed time with and without async handshake enabled.

### Monitor with OLS Real-Time Stats

OLS provides real-time statistics via the admin panel. Check:

1. Go to **Server > Real-Time Stats** in the admin panel.
2. Look at **SSL Connections** -- this shows active TLS sessions.
3. Monitor **Requests in Processing** -- with async handshake, this should stay lower under the same load.

### Check for SSL Errors

```bash
grep -i "ssl\|tls\|handshake" /usr/local/lsws/logs/error.log
```

Common entries:
- `SSL handshake completed` -- normal, successful handshake.
- `SSL handshake failed` -- client disconnected or protocol mismatch.
- `SSL async handshake queued` -- handshake was offloaded to the thread pool (expected with async enabled).

## Complete HTTPS Setup with Async Handshake

Here is a full HTTPS configuration example that includes async handshake along with modern TLS best practices:

### Step 1: Obtain an SSL Certificate

```bash
certbot certonly --webroot -w /var/www/example.com/public_html \
    -d example.com -d www.example.com \
    --non-interactive --agree-tos -m admin@example.com
```

### Step 2: Configure the Virtual Host Listener

In your OLS configuration, set up an HTTPS listener:

```
listener HTTPS {
    address                 *:443
    secure                  1
    keyFile                 /etc/letsencrypt/live/example.com/privkey.pem
    certFile                /etc/letsencrypt/live/example.com/fullchain.pem
    certChain               1

    sslProtocol             TLSv1.2 TLSv1.3
    ciphers                 ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384
    enableECDHE             1
    enableDHE               0

    enableStapling          1
    ocspRespMaxAge          86400

    map                     Example example.com, www.example.com
}
```

### Step 3: Enable Async Handshake

```
tuning {
    sslAsyncHandshake       1
    sslCryptoThreads        4
}
```

### Step 4: Enable HTTP/2 and HTTP/3

```
tuning {
    sslAsyncHandshake       1

    enableH2C               1
    enableQuic              1
}
```

HTTP/2 multiplexes many requests over a single TLS connection, reducing the number of handshakes. HTTP/3 (QUIC) uses UDP and has its own handshake mechanism that benefits from async processing.

### Step 5: Restart and Verify

```bash
# Graceful restart
/usr/local/lsws/bin/lswsctrl restart

# Verify HTTPS works
curl -I https://example.com/

# Check TLS version and cipher
openssl s_client -connect example.com:443 -servername example.com </dev/null 2>/dev/null | grep -E "Protocol|Cipher"
```

Expected output:

```
Protocol  : TLSv1.3
Cipher    : TLS_AES_256_GCM_SHA384
```

## Troubleshooting SSL Issues

### Problem: SSL handshake fails with "no shared cipher"

The client and server have no common cipher suite. Check your `ciphers` directive. For maximum compatibility while maintaining security:

```
ciphers    ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305
```

### Problem: Certificate errors in browser

- Verify the certificate chain is complete: `openssl verify -CAfile /etc/letsencrypt/live/example.com/chain.pem /etc/letsencrypt/live/example.com/cert.pem`
- Make sure `certChain 1` is set in the listener configuration.
- Check that the certificate matches the domain name in the request.

### Problem: High CPU usage from SSL

If `perf top` shows heavy CPU usage in `libcrypto` functions:
1. Enable async handshake if not already enabled.
2. Increase `sslCryptoThreads` to distribute the load.
3. Enable TLS session resumption to reduce repeat handshakes.
4. Consider using ECDSA certificates (faster than RSA for key exchange).

### Problem: Connection resets during handshake

- Check the error log for specific SSL error messages.
- Verify the private key matches the certificate: `openssl x509 -noout -modulus -in cert.pem | md5sum` should match `openssl rsa -noout -modulus -in key.pem | md5sum`.
- Ensure file permissions allow OLS to read the key file: `chmod 600 privkey.pem`.

### Problem: Async handshake enabled but no improvement

- Verify it is actually enabled: check for `SSL async handshake queued` messages in the error log.
- Your bottleneck may be elsewhere (PHP processing, database queries). Profile your application before blaming TLS.
- If most connections reuse existing TLS sessions (HTTP/2 keep-alive), there are few new handshakes to optimize.

## TLS 1.3 and HTTP/3 Considerations

### TLS 1.3

TLS 1.3 reduces the handshake from 2 round trips (TLS 1.2) to 1 round trip, and supports 0-RTT resumption. Async handshake still helps with TLS 1.3 because the cryptographic operations are the same -- they just happen in fewer round trips.

To enforce TLS 1.3 only (if all your clients support it):

```
sslProtocol    TLSv1.3
```

For broad compatibility, allow both 1.2 and 1.3:

```
sslProtocol    TLSv1.2 TLSv1.3
```

### HTTP/3 (QUIC)

HTTP/3 runs over QUIC (UDP) and uses TLS 1.3 natively. The QUIC handshake integrates the TLS handshake, making 0-RTT connections possible. Async processing benefits QUIC connections just as it does traditional TLS connections.

To enable HTTP/3 in OLS:

```
tuning {
    enableQuic    1
}

listener HTTPS {
    ...
    enableQuic    1
}
```

Verify HTTP/3 is working:

```bash
curl --http3 -I https://example.com/
```

Note: HTTP/3 requires the client to support it. Modern browsers (Chrome, Firefox, Safari) all support HTTP/3. The `alt-svc` response header advertises HTTP/3 availability to clients:

```
alt-svc: h3=":443"; ma=86400
```

OLS sends this header automatically when QUIC is enabled, allowing clients to upgrade to HTTP/3 on subsequent connections.
