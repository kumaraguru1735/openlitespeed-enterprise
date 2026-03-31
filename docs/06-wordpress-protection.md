# WordPress Brute Force Protection Guide

## What Is mod_wpprotect?

`mod_wpprotect` is a built-in OpenLiteSpeed module that protects WordPress login pages (`wp-login.php`) from brute force attacks. It works at the web server level, meaning malicious requests are blocked before they ever reach PHP or WordPress.

The module tracks failed login attempts per IP address using shared memory. When an IP exceeds a configurable threshold of failed attempts, the module either blocks the IP entirely or presents a Google reCAPTCHA challenge. This happens without any WordPress plugin -- the protection is handled by OLS itself.

## How It Works

1. A visitor requests `wp-login.php` with a POST (login attempt).
2. OLS increments a counter for that visitor's IP address in shared memory.
3. If the counter exceeds the configured threshold within the time window, the module activates.
4. Depending on your configuration, the module either:
   - **Deny mode**: Returns a 403 Forbidden response (hard block).
   - **Captcha mode**: Redirects the visitor to a reCAPTCHA challenge page. If they pass, they can proceed to login.
5. After the lockout period expires, the IP's counter resets.

The shared memory approach means there is virtually zero performance overhead during normal operations -- no database queries, no file reads, no PHP execution.

## Step-by-Step Setup

### Step 1: Enable the Module

The module is compiled into this OLS build. To enable it, add the module configuration to your OLS config.

In the OLS admin panel (default: https://your-server:7080):

1. Go to **Server Configuration > Modules**.
2. Click **Add** to register a new module.
3. Set the module name to `mod_wpprotect`.

Or edit the configuration file directly:

```
module mod_wpprotect {
    enabled             1
    wpProtectAction     2
    wpProtectLimit      10
}
```

### Step 2: Get Google reCAPTCHA Keys

If you want to use Captcha mode (recommended over hard deny), you need reCAPTCHA v2 keys from Google:

1. Go to https://www.google.com/recaptcha/admin/create
2. Enter a label (e.g., "My WordPress Site").
3. Select **reCAPTCHA v2** and then **"I'm not a robot" Checkbox**.
4. Add your domain(s) under "Domains" (e.g., `example.com`).
5. Accept the terms and click **Submit**.
6. You will receive two keys:
   - **Site Key**: Used in the HTML form (public, shown to visitors).
   - **Secret Key**: Used for server-side verification (keep private).

Save both keys -- you will need them in the next step.

### Step 3: Configure the Module

Add the full configuration block to your virtual host or server-level config:

```
module mod_wpprotect {
    enabled             1
    wpProtectAction     2
    wpProtectLimit      10

    wpProtectLockout    300
    wpProtectWindow     60

    recaptchaSiteKey    YOUR_RECAPTCHA_SITE_KEY_HERE
    recaptchaSecretKey  YOUR_RECAPTCHA_SECRET_KEY_HERE
    recaptchaType       1
    recaptchaMaxTries   3
}
```

Replace `YOUR_RECAPTCHA_SITE_KEY_HERE` and `YOUR_RECAPTCHA_SECRET_KEY_HERE` with the keys from Step 2.

### Step 4: Restart OLS and Test

```bash
# Restart OpenLiteSpeed
systemctl restart lsws

# Or use the graceful restart
/usr/local/lsws/bin/lswsctrl restart
```

**Testing the protection:**

```bash
# Send multiple rapid login attempts (will trigger protection)
for i in $(seq 1 15); do
    curl -s -o /dev/null -w "%{http_code}\n" \
        -X POST https://example.com/wp-login.php \
        -d "log=admin&pwd=wrongpassword&wp-submit=Log+In"
done
```

After 10 attempts (the configured limit), you should see:
- **Deny mode (action=1)**: HTTP 403 responses.
- **Captcha mode (action=2)**: HTTP 302 redirect to the reCAPTCHA page.

## Configuration Options

| Parameter            | Default | Description                                                    |
|----------------------|---------|----------------------------------------------------------------|
| `enabled`            | 0       | Master switch. Set to `1` to enable the module.                |
| `wpProtectAction`    | 1       | What to do when limit is reached. `1` = deny, `2` = captcha.  |
| `wpProtectLimit`     | 10      | Number of failed attempts before action triggers.              |
| `wpProtectLockout`   | 300     | Seconds an IP is locked out after exceeding the limit.         |
| `wpProtectWindow`    | 60      | Time window in seconds for counting failed attempts.           |
| `recaptchaSiteKey`   | (none)  | Google reCAPTCHA v2 site key (required for captcha mode).      |
| `recaptchaSecretKey` | (none)  | Google reCAPTCHA v2 secret key (required for captcha mode).    |
| `recaptchaType`      | 1       | reCAPTCHA type. `1` = checkbox ("I'm not a robot").            |
| `recaptchaMaxTries`  | 3       | Max reCAPTCHA attempts before falling back to deny.            |

## reCAPTCHA Flow Explained

When Captcha mode is active and an IP exceeds the login attempt threshold, here is what happens from the visitor's perspective:

1. The visitor tries to log into WordPress and fails multiple times.
2. After exceeding the limit, their next request to `wp-login.php` is intercepted by OLS.
3. Instead of showing the WordPress login page, OLS returns a page with a Google reCAPTCHA checkbox challenge.
4. The visitor completes the reCAPTCHA ("I'm not a robot" checkbox).
5. OLS verifies the reCAPTCHA response with Google's servers using the secret key.
6. If verification passes, the visitor is allowed through to the real WordPress login page.
7. If the visitor fails the reCAPTCHA multiple times (up to `recaptchaMaxTries`), they are hard-blocked with a 403.

The reCAPTCHA page is generated by OLS itself -- it does not depend on WordPress or any plugin.

## Deny Mode vs Captcha Mode

| Aspect                  | Deny Mode (`action=1`)          | Captcha Mode (`action=2`)         |
|-------------------------|---------------------------------|-----------------------------------|
| User experience         | Hard block, 403 error           | reCAPTCHA challenge, then access  |
| Legitimate user impact  | Locked out if they mistype      | Can prove they are human          |
| Bot effectiveness       | Stops all bots                  | Stops most bots (some solve CAPTCHAs) |
| Configuration effort    | Minimal (no reCAPTCHA keys)     | Requires Google reCAPTCHA setup   |
| Recommended for         | Sites with few admins           | Sites with many contributors      |

**Recommendation:** Use Captcha mode (`action=2`) for production sites. It is friendlier to legitimate users who may mistype their password a few times, while still blocking automated attacks effectively.

## Whitelisting

### Admin Cookie Whitelisting

If an administrator is already logged into WordPress (has a valid `wordpress_logged_in_*` cookie), the module does not count their requests against the threshold. This prevents admins from accidentally locking themselves out during testing.

### Trusted IP Whitelisting

To whitelist specific IPs (e.g., your office IP), add them to the OLS access control configuration at the virtual host level:

```
accessControl {
    allow                 192.168.1.0/24, 10.0.0.5
    deny                  ALL
}
```

Note: Whitelisted IPs bypass the brute force protection entirely. Only whitelist IPs you fully trust.

### CloudFlare Considerations

If your site is behind CloudFlare, OLS sees CloudFlare's IP addresses instead of real visitor IPs. To fix this:

1. Enable the "Use Client IP in Header" option in OLS.
2. Set the header to `X-Forwarded-For` or `CF-Connecting-IP`.
3. Add CloudFlare's IP ranges to the trusted proxy list.

Without this, all visitors appear to come from the same IP, and one person's failed logins could lock out everyone.

## Monitoring Blocked Attempts

### Check OLS Error Log

Blocked attempts are logged in the OLS error log:

```bash
tail -f /usr/local/lsws/logs/error.log | grep -i wpprotect
```

You will see entries like:

```
[mod_wpprotect] IP 203.0.113.42 exceeded threshold (10), action: captcha
[mod_wpprotect] IP 198.51.100.7 exceeded threshold (10), action: deny
[mod_wpprotect] IP 203.0.113.42 passed reCAPTCHA verification
```

### Check Access Log for Patterns

Look for repeated POST requests to `wp-login.php`:

```bash
grep "POST /wp-login.php" /usr/local/lsws/logs/access.log | \
    awk '{print $1}' | sort | uniq -c | sort -rn | head -20
```

This shows the top 20 IPs by number of login attempts. IPs with hundreds or thousands of attempts are almost certainly bots.

### Real-Time Monitoring

```bash
# Watch for login attempts in real time
tail -f /usr/local/lsws/logs/access.log | grep "wp-login.php"
```

## Complete Production Configuration Example

Here is a complete, production-ready configuration for a WordPress site with brute force protection:

```
module mod_wpprotect {
    # Enable the module
    enabled             1

    # Use captcha mode (friendlier than hard deny)
    wpProtectAction     2

    # Allow 5 failed attempts within 60 seconds before triggering
    wpProtectLimit      5
    wpProtectWindow     60

    # Lock out offending IPs for 10 minutes
    wpProtectLockout    600

    # Google reCAPTCHA v2 credentials
    recaptchaSiteKey    6LeIxAcTAAAAAJcZVRqyHh71UMIEGNQ_MXjiZKhI
    recaptchaSecretKey  6LeIxAcTAAAAAGG-vFI1TnRWxMZNFuojJ4WifJWe

    # reCAPTCHA v2 checkbox type
    recaptchaType       1

    # Allow 3 reCAPTCHA attempts before hard deny
    recaptchaMaxTries   3
}
```

Note: The reCAPTCHA keys shown above are Google's public test keys. Replace them with your own keys for production use.

## Frequently Asked Questions

**Does mod_wpprotect work with WooCommerce?**

Yes. WooCommerce uses the standard WordPress login system (`wp-login.php`), so the protection applies to WooCommerce customer logins as well. Note that if you have a custom login page (e.g., `/my-account/`), the module only protects the standard `wp-login.php` endpoint. WooCommerce's `/my-account/` login form submits to `wp-login.php` by default, so it is covered.

**Does it work with CloudFlare?**

Yes, but you must configure OLS to read the real visitor IP from CloudFlare's headers (see the CloudFlare section above). Without this, the module sees CloudFlare's IP instead of the visitor's IP, which renders per-IP tracking useless.

**Does it work with Wordfence or other security plugins?**

Yes. mod_wpprotect operates at the web server level, before WordPress loads. Security plugins like Wordfence operate at the PHP/WordPress level. They complement each other -- mod_wpprotect blocks attacks before PHP is invoked, reducing server load, while Wordfence provides additional application-level protections.

**Will it block XML-RPC brute force attacks?**

The module specifically targets `wp-login.php`. XML-RPC (`xmlrpc.php`) is a separate endpoint. To protect XML-RPC, you can either disable it entirely (recommended if not needed) or add a separate rate-limiting rule in your OLS configuration.

**What happens if I forget my password and trigger the lockout?**

If you are using Captcha mode, you simply solve the reCAPTCHA and proceed. If you are using Deny mode, you will need to wait for the lockout period to expire (default 5 minutes) or access the server directly to whitelist your IP.

**Does it affect wp-admin pages?**

No. The module only monitors POST requests to `wp-login.php`. Once you are logged in, accessing `/wp-admin/` pages is not affected.

**Can I change the threshold per virtual host?**

Yes. The module configuration can be set at both the server level (applies to all virtual hosts) and the virtual host level (overrides server-level settings for that specific host).

**How much memory does it use?**

The shared memory segment used for IP tracking is very small -- typically a few megabytes even with thousands of tracked IPs. Each IP entry is automatically cleaned up after the lockout window expires.

**Does it log blocked attempts?**

Yes. All blocked attempts and reCAPTCHA verifications are logged in the OLS error log at `/usr/local/lsws/logs/error.log`. See the Monitoring section above.
