# ESI (Edge Side Includes) Guide

A complete, beginner-friendly guide to using Edge Side Includes with OpenLiteSpeed. Includes architecture explanation, all ESI tags, real-world examples, and troubleshooting.

---

## Table of Contents

1. [What ESI Is and Why You Need It](#what-esi-is-and-why-you-need-it)
2. [How ESI Works in OpenLiteSpeed](#how-esi-works-in-openlitespeed)
3. [Enabling ESI](#enabling-esi)
4. [All ESI Tags Explained](#all-esi-tags-explained)
   - [esi:include](#esiinclude)
   - [esi:remove](#esiremove)
   - [esi:comment](#esicomment)
   - [esi:try / esi:attempt / esi:except](#esitry--esiattempt--esiexcept)
5. [Complete Example: Product Page with Dynamic Cart](#complete-example-product-page-with-dynamic-cart)
6. [Complete Example: WordPress Sidebar Caching](#complete-example-wordpress-sidebar-caching)
7. [Using ESI with LSCache](#using-esi-with-lscache)
8. [Performance Tuning](#performance-tuning)
9. [Troubleshooting ESI](#troubleshooting-esi)
10. [PHP Code Examples](#php-code-examples)

---

## What ESI Is and Why You Need It

**Edge Side Includes (ESI)** is a markup language that lets you break a web page into fragments, each with its own caching rules. The web server assembles the final page from these fragments at the edge (i.e., at the server level) before sending it to the browser.

**The problem ESI solves:**

Imagine a product page that is 95% static (product name, description, images) and 5% dynamic (shopping cart count, logged-in username). Without ESI, you have two bad choices:

1. **Cache the whole page** -- the cart and username are wrong for every visitor.
2. **Don't cache at all** -- your server generates the entire page on every request, wasting resources.

**With ESI, you get the best of both worlds:**

- Cache the static 95% of the page (product details).
- Fetch only the dynamic 5% (cart widget) on every request.
- The server assembles them into one page and sends it to the browser.

The browser sees a normal HTML page and has no idea ESI was involved.

---

## How ESI Works in OpenLiteSpeed

Here is the processing flow, step by step:

```
                        [1] Browser requests /product/123
                                    |
                                    v
                    [2] OpenLiteSpeed receives the request
                                    |
                                    v
           [3] OLS checks cache --> Cache HIT for /product/123
                |                           |
                | (miss)                    v
                v                  [4] Return cached body
        [3b] Forward to PHP                 |
                |                           v
                v              [5] OLS scans body for <esi:...> tags
        [3c] PHP generates                  |
         response with ESI         Found esi:include tags?
         tags + header                 /           \
                |                 NO /             \ YES
                v                  v               v
        [3d] OLS caches      [6] Send as-is   [7] For each esi:include:
         the response                              fetch the fragment via
                |                                  internal subrequest
                v                                       |
        [5] OLS scans body                              v
         for <esi:...> tags                    [8] Assemble: replace each
                                                esi:include tag with the
                                                fragment response body
                                                        |
                                                        v
                                               [9] Send assembled page
                                                    to browser
```

Key points:

- ESI processing happens **inside the server**, not in the browser.
- ESI fragments are fetched via **internal subrequests** (local HTTP requests to the same server).
- The maximum response body size for ESI processing is **8 MB** by default.
- The maximum fragment size is **2 MB** by default.
- The maximum number of ESI nodes (tags) per response is **1024**.

---

## Enabling ESI

ESI is implemented as the `mod_esi` module in OpenLiteSpeed. It is activated on a **per-response basis** using a special HTTP response header.

### Step 1 -- Ensure the module is compiled

The ESI module is compiled by default with OpenLiteSpeed. You can verify it exists:

```bash
ls /usr/local/lsws/modules/mod_esi.so
```

### Step 2 -- Activate ESI via response header

Your application must send this response header to tell OLS to process ESI tags:

```
X-LiteSpeed-Cache-Control: esi=on
```

In PHP:

```php
header('X-LiteSpeed-Cache-Control: esi=on');
```

**This header is the on/off switch.** If the header is not present, OLS passes the response through without scanning for ESI tags. This means ESI processing has zero overhead for pages that do not use it.

### Step 3 -- Use ESI tags in your HTML

Once the activation header is sent, you can use ESI tags anywhere in your HTML output:

```html
<html>
<body>
    <h1>Product Name</h1>
    <div class="cart-widget">
        <esi:include src="/fragments/cart-count.php" />
    </div>
    <p>Product description here...</p>
</body>
</html>
```

---

## All ESI Tags Explained

### esi:include

The most important ESI tag. It fetches a fragment from another URL and inserts its content inline.

**Basic usage:**

```html
<esi:include src="/fragments/header.php" />
```

OLS makes an internal subrequest to `/fragments/header.php`, gets the response body, and replaces the `<esi:include>` tag with that body.

**With a fallback URL (alt):**

```html
<esi:include src="/fragments/live-stock.php" alt="/fragments/stock-fallback.html" />
```

If `/fragments/live-stock.php` fails (returns an error), OLS tries `/fragments/stock-fallback.html` instead.

**With error handling (onerror):**

```html
<esi:include src="/fragments/optional-widget.php" onerror="continue" />
```

If the fragment request fails and `onerror="continue"` is set, OLS silently removes the tag and continues. Without this attribute, a failed include causes the entire page to return an error.

**Complete example with all attributes:**

```html
<esi:include
    src="/fragments/sidebar.php?section=news"
    alt="/fragments/sidebar-static.html"
    onerror="continue"
/>
```

**Rules for src URLs:**

- Must be **local URIs** (same server). External URLs are not supported.
- Can include query strings.
- Must start with `/`.
- Maximum URL length is 4096 characters.

### esi:remove

Content inside `<esi:remove>` tags is stripped from the response when ESI is active. This is useful for providing fallback content for browsers or systems that do not process ESI.

```html
<div class="cart">
    <esi:include src="/fragments/cart.php" />
    <esi:remove>
        <p>Shopping cart is loading...</p>
    </esi:remove>
</div>
```

**How it works:**

- When ESI is **active**: The `<esi:remove>` block and everything inside it is deleted. The `<esi:include>` is processed normally.
- When ESI is **not active** (e.g., during development without a server): The `<esi:remove>` tags are unknown to the browser and ignored, so the fallback text "Shopping cart is loading..." is displayed.

This lets you have meaningful content visible during development or if ESI processing is disabled.

### esi:comment

A comment tag that is completely removed from the output. Useful for documentation that should not appear in the final HTML.

```html
<esi:comment text="This section is cached for 1 hour" />

<esi:comment text="TODO: Add personalization fragment here" />
```

Unlike HTML comments (`<!-- -->`), `esi:comment` tags are **never** sent to the browser. They are removed during ESI processing.

### esi:try / esi:attempt / esi:except

These tags provide structured error handling for ESI includes. They work like a try/catch block in programming.

**Syntax:**

```html
<esi:try>
    <esi:attempt>
        <!-- This is tried first -->
        <esi:include src="/fragments/dynamic-content.php" />
    </esi:attempt>
    <esi:except>
        <!-- This is used if the attempt fails -->
        <p>Content is temporarily unavailable.</p>
    </esi:except>
</esi:try>
```

**How it works:**

1. OLS processes everything inside `<esi:attempt>`.
2. If all includes inside `<esi:attempt>` succeed, the content from `<esi:attempt>` is used and `<esi:except>` is discarded.
3. If any include inside `<esi:attempt>` fails, the entire `<esi:attempt>` block is discarded and `<esi:except>` is used instead.

**When to use try/attempt/except vs. onerror="continue":**

- Use `onerror="continue"` when you want to silently skip a failed fragment.
- Use `try/attempt/except` when you want to show alternative content on failure.

**Real-world example:**

```html
<esi:try>
    <esi:attempt>
        <div class="recommendations">
            <h3>Recommended for you</h3>
            <esi:include src="/fragments/personalized-recs.php" />
        </div>
    </esi:attempt>
    <esi:except>
        <div class="recommendations">
            <h3>Popular items</h3>
            <esi:include src="/fragments/popular-items.php" />
        </div>
    </esi:except>
</esi:try>
```

If the personalized recommendations engine is down, visitors see popular items instead.

---

## Complete Example: Product Page with Dynamic Cart

This example shows an e-commerce product page where the product details are cached but the cart widget is always fresh.

### product.php (the main page)

```php
<?php
// Tell OLS to process ESI tags and cache this page for 1 hour
header('X-LiteSpeed-Cache-Control: esi=on,public,max-age=3600');

$product = getProduct($_GET['id']); // Your product lookup logic
?>
<!DOCTYPE html>
<html>
<head>
    <title><?= htmlspecialchars($product['name']) ?></title>
</head>
<body>
    <header>
        <nav>
            <a href="/">Home</a>
            <div class="user-area">
                <!-- This fragment is private (per-user) and never cached -->
                <esi:include src="/fragments/user-menu.php" />
            </div>
            <div class="cart-count">
                <!-- This fragment is private and refreshed every request -->
                <esi:include src="/fragments/cart-count.php" />
                <esi:remove>
                    <span>Cart</span>
                </esi:remove>
            </div>
        </nav>
    </header>

    <main>
        <!-- All of this is part of the cached page -->
        <h1><?= htmlspecialchars($product['name']) ?></h1>
        <img src="<?= $product['image'] ?>" alt="<?= htmlspecialchars($product['name']) ?>">
        <p class="price">$<?= number_format($product['price'], 2) ?></p>
        <p><?= $product['description'] ?></p>

        <esi:comment text="Recommendations are cached separately for 30 min" />
        <div class="recommendations">
            <esi:try>
                <esi:attempt>
                    <esi:include src="/fragments/recommendations.php?id=<?= $product['id'] ?>" />
                </esi:attempt>
                <esi:except>
                    <p>Check out our <a href="/popular">popular items</a>.</p>
                </esi:except>
            </esi:try>
        </div>
    </main>
</body>
</html>
```

### fragments/cart-count.php

```php
<?php
// This fragment is private (per-user) and not cached
header('X-LiteSpeed-Cache-Control: no-cache');

session_start();
$count = isset($_SESSION['cart']) ? count($_SESSION['cart']) : 0;
echo "Cart ($count)";
```

### fragments/recommendations.php

```php
<?php
// Cache recommendations for 30 minutes
header('X-LiteSpeed-Cache-Control: public,max-age=1800');

$productId = (int)$_GET['id'];
$recs = getRecommendations($productId); // Your logic
foreach ($recs as $rec) {
    echo '<div class="rec-item">';
    echo '<a href="/product/' . $rec['id'] . '">' . htmlspecialchars($rec['name']) . '</a>';
    echo '</div>';
}
```

---

## Complete Example: WordPress Sidebar Caching

This example shows how to use ESI to cache a WordPress page while keeping the sidebar dynamic.

### In your theme's single.php (or page.php)

```php
<?php
// Send ESI activation header
header('X-LiteSpeed-Cache-Control: esi=on,public,max-age=3600');

get_header();
?>

<div class="content-area">
    <main>
        <?php while (have_posts()) : the_post(); ?>
            <article>
                <h1><?php the_title(); ?></h1>
                <?php the_content(); ?>
            </article>
        <?php endwhile; ?>
    </main>

    <aside class="sidebar">
        <esi:comment text="Sidebar is fetched fresh; it has recent posts and login status" />
        <esi:try>
            <esi:attempt>
                <esi:include src="/esi-fragments/sidebar.php" />
            </esi:attempt>
            <esi:except>
                <?php get_sidebar(); ?>
            </esi:except>
        </esi:try>
    </aside>
</div>

<?php get_footer(); ?>
```

### esi-fragments/sidebar.php

```php
<?php
// Do not cache the sidebar fragment (or cache it for a short time)
header('X-LiteSpeed-Cache-Control: private,max-age=60');

// Load WordPress
require_once dirname(__DIR__) . '/wp-load.php';

// Display recent posts
$recent = wp_get_recent_posts(['numberposts' => 5]);
echo '<div class="widget recent-posts">';
echo '<h3>Recent Posts</h3><ul>';
foreach ($recent as $post) {
    echo '<li><a href="' . get_permalink($post['ID']) . '">';
    echo esc_html($post['post_title']);
    echo '</a></li>';
}
echo '</ul></div>';

// Display login/logout
echo '<div class="widget login-status">';
if (is_user_logged_in()) {
    $user = wp_get_current_user();
    echo 'Hello, ' . esc_html($user->display_name);
    echo ' | <a href="' . wp_logout_url() . '">Log out</a>';
} else {
    echo '<a href="' . wp_login_url() . '">Log in</a>';
}
echo '</div>';
```

---

## Using ESI with LSCache

ESI and LSCache work together naturally. LSCache is the built-in page caching module in OpenLiteSpeed.

### How they interact

1. LSCache caches the **entire response** including the ESI tags (not the assembled output).
2. When a cached page is served, OLS still processes ESI tags and fetches fragments.
3. ESI fragments themselves can also be cached by LSCache, each with their own TTL.

### Cache header reference

| Header | Purpose |
|--------|---------|
| `X-LiteSpeed-Cache-Control: esi=on` | Enable ESI processing for this response |
| `X-LiteSpeed-Cache-Control: public,max-age=3600` | Cache this page publicly for 1 hour |
| `X-LiteSpeed-Cache-Control: private,max-age=300` | Cache per-user for 5 minutes |
| `X-LiteSpeed-Cache-Control: no-cache` | Do not cache this response |

### Combined headers

You can combine ESI activation with caching in one header:

```php
// Cache the page publicly for 1 hour and enable ESI
header('X-LiteSpeed-Cache-Control: esi=on,public,max-age=3600');
```

### Cache configuration in httpd_config.conf

Make sure the cache module is enabled:

```
module cache {
    ls_enabled              1
    checkPrivateCache       1
    checkPublicCache        1
    maxCacheObjSize         10000000
    maxStaleAge             200
    qsCache                 1
    reqCookieCache          1
    respCookieCache         1
    ignoreReqCacheCtrl      1
    ignoreRespCacheCtrl     0
    enableCache             0
    expireInSeconds         3600
    enablePrivateCache      0
    privateExpireInSeconds  3600
}
```

---

## Performance Tuning

### Maximum body size

The ESI module only processes response bodies up to **8 MB** (`ESI_MAX_BODY_SIZE`). Responses larger than this bypass ESI processing entirely. This is a compile-time limit defined in the source code. If you need to process larger responses, you would need to modify `ESI_MAX_BODY_SIZE` in `src/modules/esi/mod_esi.cpp` and recompile.

### Maximum fragment size

Each ESI fragment (the response from an `esi:include` subrequest) is limited to **2 MB** (`ESI_MAX_FRAGMENT_SIZE`). Keep fragments small for best performance.

### Maximum number of ESI tags

A single response can contain up to **1024** ESI tags (`ESI_MAX_NODES`). This is more than enough for any real-world page.

### Maximum tag length

A single ESI tag (including all attributes) can be up to **8192** characters (`ESI_MAX_TAG_LEN`).

### Maximum URL length

The `src` and `alt` attribute URLs can be up to **4096** characters (`ESI_MAX_URL_LEN`).

### Performance best practices

1. **Keep fragments small.** Each `esi:include` triggers an internal subrequest. Aim for fragments under 10 KB.

2. **Minimize the number of includes.** Each include adds latency. Combine related dynamic content into one fragment when possible.

3. **Cache fragments when possible.** A fragment that changes every 60 seconds can still be cached for 60 seconds, saving hundreds of regenerations per minute.

4. **Use onerror="continue" for non-critical fragments.** This prevents an optional widget from breaking the entire page.

5. **Avoid nested ESI.** While the parser supports multiple tags, deeply assembling fragments that themselves contain ESI tags adds complexity. Keep ESI to one level deep.

6. **Profile with response headers.** Check the response time with and without ESI to measure the overhead. The overhead should be minimal (a few milliseconds) for pages with a handful of includes.

---

## Troubleshooting ESI

### ESI tags appear as plain text in the browser

**Cause:** The ESI activation header is missing.

**Fix:** Make sure your application sends the header before any output:

```php
header('X-LiteSpeed-Cache-Control: esi=on');
```

Check that no output was sent before this line (no whitespace, no BOM, no echo). Use `headers_sent()` to debug:

```php
if (headers_sent($file, $line)) {
    error_log("Headers already sent in $file on line $line");
}
```

### ESI include returns empty content

**Cause:** The fragment URL returned an error or empty response.

**Fix:**

1. Test the fragment URL directly in a browser: `http://yoursite.com/fragments/widget.php`
2. Check the error log for subrequest failures:

   ```bash
   tail -100 /usr/local/lsws/logs/error.log | grep -i esi
   ```

3. Make sure the fragment URL starts with `/` and is on the same server.

### ESI page returns a 500 error

**Cause:** An ESI include failed and `onerror` was not set to "continue".

**Fix:** Either fix the failing fragment or add error handling:

```html
<esi:include src="/fragments/widget.php" onerror="continue" />
```

Or use try/attempt/except for a fallback:

```html
<esi:try>
    <esi:attempt>
        <esi:include src="/fragments/widget.php" />
    </esi:attempt>
    <esi:except>
        <p>Default content</p>
    </esi:except>
</esi:try>
```

### ESI processing is slow

**Cause:** Too many includes, or fragment generation is slow.

**Fix:**

1. Reduce the number of `esi:include` tags. Combine related fragments.
2. Cache fragments with appropriate TTLs.
3. Make fragment endpoints fast (lightweight PHP scripts, not full framework boots).
4. Check that fragment URLs are not triggering external API calls or heavy database queries.

### ESI tags are processed but the page is not cached

**Cause:** The cache control header is missing or incorrect.

**Fix:** You need both ESI activation and cache control:

```php
// Both in one header
header('X-LiteSpeed-Cache-Control: esi=on,public,max-age=3600');
```

Also check that the cache module is enabled in `httpd_config.conf`:

```
module cache {
    ls_enabled      1
    enableCache     1
}
```

---

## PHP Code Examples

### Helper class for ESI in PHP

```php
<?php

class ESI
{
    /**
     * Send the ESI activation header.
     * Call this at the top of any page that uses ESI tags.
     *
     * @param int $maxAge  Cache time in seconds (0 = no cache)
     * @param bool $public Whether the cache is public or per-user
     */
    public static function activate(int $maxAge = 3600, bool $public = true): void
    {
        $visibility = $public ? 'public' : 'private';
        $cache = $maxAge > 0 ? "{$visibility},max-age={$maxAge}" : 'no-cache';
        header("X-LiteSpeed-Cache-Control: esi=on,{$cache}");
    }

    /**
     * Output an esi:include tag.
     *
     * @param string $src     The fragment URL (must start with /)
     * @param string $alt     Optional fallback URL
     * @param bool   $continue  If true, silently skip on failure
     */
    public static function include(
        string $src,
        string $alt = '',
        bool $continue = false
    ): void {
        $tag = '<esi:include src="' . htmlspecialchars($src, ENT_QUOTES) . '"';
        if ($alt !== '') {
            $tag .= ' alt="' . htmlspecialchars($alt, ENT_QUOTES) . '"';
        }
        if ($continue) {
            $tag .= ' onerror="continue"';
        }
        $tag .= ' />';
        echo $tag;
    }

    /**
     * Output a block that is removed when ESI is active.
     * Useful for fallback content.
     *
     * @param string $fallbackHtml  HTML to show when ESI is not active
     */
    public static function remove(string $fallbackHtml): void
    {
        echo '<esi:remove>' . $fallbackHtml . '</esi:remove>';
    }

    /**
     * Output an ESI comment (removed from final output).
     */
    public static function comment(string $text): void
    {
        echo '<esi:comment text="' . htmlspecialchars($text, ENT_QUOTES) . '" />';
    }

    /**
     * Mark this fragment response as non-cacheable.
     * Call this in your fragment scripts.
     */
    public static function noCache(): void
    {
        header('X-LiteSpeed-Cache-Control: no-cache');
    }

    /**
     * Mark this fragment response as cacheable.
     * Call this in your fragment scripts.
     */
    public static function cacheFragment(int $maxAge = 300, bool $public = true): void
    {
        $visibility = $public ? 'public' : 'private';
        header("X-LiteSpeed-Cache-Control: {$visibility},max-age={$maxAge}");
    }
}
```

### Using the helper

**Main page:**

```php
<?php
require_once 'ESI.php';

ESI::activate(3600, true); // Cache page for 1 hour, public
?>
<!DOCTYPE html>
<html>
<body>
    <header>
        <?php ESI::include('/fragments/nav.php', '', true); ?>
        <?php ESI::remove('<nav>Loading navigation...</nav>'); ?>
    </header>

    <main>
        <h1>Welcome to our site</h1>
        <?php ESI::comment('User dashboard widget below'); ?>
        <?php ESI::include('/fragments/dashboard.php', '/fragments/dashboard-guest.html'); ?>
    </main>
</body>
</html>
```

**Fragment script (fragments/nav.php):**

```php
<?php
require_once dirname(__DIR__) . '/ESI.php';

ESI::cacheFragment(300, false); // Cache per-user for 5 minutes

session_start();
$name = $_SESSION['username'] ?? 'Guest';
?>
<nav>
    <a href="/">Home</a>
    <a href="/products">Products</a>
    <span>Hello, <?= htmlspecialchars($name) ?></span>
</nav>
```
