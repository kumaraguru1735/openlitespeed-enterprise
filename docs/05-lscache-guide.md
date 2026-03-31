# LSCache Complete Guide

## What Is LSCache?

LSCache (LiteSpeed Cache) is a **server-level page caching** engine built directly into OpenLiteSpeed. Unlike application-level caching solutions (Redis object cache, Memcached, or PHP opcode caches), LSCache operates at the web server layer. When a page is cached, OLS serves it directly from memory without invoking PHP at all.

Think of it this way:

- **Application-level cache** (e.g., Redis): PHP still starts, your framework boots, then Redis returns a cached value. The request still hits your application.
- **LSCache (server-level cache)**: OLS intercepts the request before PHP even starts. If a cached copy exists, OLS returns it immediately. PHP never runs.

This difference is why LSCache can serve thousands of requests per second on modest hardware -- cached responses bypass the entire application stack.

## Default Cache Limits

This OLS build ships with tuned defaults that are higher than stock OpenLiteSpeed:

| Setting        | Stock OLS Default | This Build's Default | What It Controls                          |
|----------------|-------------------|----------------------|-------------------------------------------|
| `maxObjSize`   | 10MB              | **256MB**            | Maximum size of a single cached object    |
| `maxStale`     | 200 seconds       | **86400** (24 hours) | How long stale cache can be served        |
| `litemage`     | off               | **on**               | LiteMage (full-page cache for Magento)    |

These raised limits mean you can cache larger pages (e.g., product catalogs with many images) and serve stale content for longer during backend issues.

## Enabling LSCache for WordPress

WordPress with LSCache is the most common and best-supported setup.

### Step 1: Install the LiteSpeed Cache Plugin

1. Log into your WordPress admin dashboard.
2. Go to **Plugins > Add New**.
3. Search for **"LiteSpeed Cache"**.
4. Click **Install Now**, then **Activate**.

### Step 2: Verify Server-Level Cache Is Working

After activating the plugin, visit your site in a browser and check the response headers:

```bash
curl -I https://example.com/
```

Look for these headers in the response:

```
X-LiteSpeed-Cache: hit
```

If you see `hit`, the page was served from LSCache. If you see `miss`, the page was just cached and will be a `hit` on the next request.

### Step 3: Configure the Plugin

In your WordPress admin, go to **LiteSpeed Cache > General**:

- **Enable Cache**: ON
- **Cache Logged-in Users**: OFF (recommended for most sites)
- **Cache Commenters**: OFF
- **Cache REST API**: ON
- **Cache Login Page**: ON

Under **LiteSpeed Cache > Cache > TTL**:

- **Default Public Cache TTL**: 604800 (1 week) for mostly-static sites
- **Default Private Cache TTL**: 1800 (30 minutes)
- **Default Front Page TTL**: 604800

### Step 4: Test Your Setup

```bash
# First request -- will be a "miss" (page gets cached)
curl -s -o /dev/null -D - https://example.com/ | grep X-LiteSpeed

# Second request -- should be a "hit"
curl -s -o /dev/null -D - https://example.com/ | grep X-LiteSpeed
```

Expected output on second request:

```
X-LiteSpeed-Cache: hit
X-LiteSpeed-Cache-Control: public,max-age=604800
```

## Enabling LSCache for Laravel

Laravel does not have an official LSCache plugin, but you can enable it with response headers.

### Option 1: Middleware (Recommended)

Create a middleware that adds LSCache headers:

```php
<?php
// app/Http/Middleware/LsCache.php

namespace App\Http\Middleware;

use Closure;
use Illuminate\Http\Request;

class LsCache
{
    public function handle(Request $request, Closure $next, string $ttl = '3600')
    {
        $response = $next($request);

        // Only cache GET requests that return 200
        if ($request->isMethod('GET') && $response->getStatusCode() === 200) {
            // Don't cache authenticated user pages by default
            if (!auth()->check()) {
                $response->header('X-LiteSpeed-Cache-Control', "public,max-age={$ttl}");
                $response->header('X-LiteSpeed-Tag', 'laravel');
            }
        }

        return $response;
    }
}
```

Register it in your route file:

```php
// routes/web.php
Route::middleware(['lscache:7200'])->group(function () {
    Route::get('/', [HomeController::class, 'index']);
    Route::get('/about', [PageController::class, 'about']);
    Route::get('/products', [ProductController::class, 'index']);
});
```

## Enabling LSCache for Any PHP App

For any PHP application, you control LSCache entirely through HTTP response headers:

```php
<?php
if (empty($_SESSION['user_id'])) {
    // Cache public pages for 2 hours
    header('X-LiteSpeed-Cache-Control: public,max-age=7200');
    header('X-LiteSpeed-Tag: page_' . basename($_SERVER['REQUEST_URI']));
} else {
    // Don't cache logged-in user pages
    header('X-LiteSpeed-Cache-Control: no-cache');
}
echo "<h1>Welcome to my site</h1>";
```

To purge cached content when it changes, send a purge header in your response:

```php
header('X-LiteSpeed-Purge: tag=product:42');
```

## Cache Control Headers Explained

### X-LiteSpeed-Cache-Control

This is the primary header that tells OLS how to cache a response.

```
X-LiteSpeed-Cache-Control: public,max-age=3600
X-LiteSpeed-Cache-Control: private,max-age=1800
X-LiteSpeed-Cache-Control: no-cache
X-LiteSpeed-Cache-Control: no-vary
```

| Directive    | Meaning                                                    |
|--------------|------------------------------------------------------------|
| `public`     | Cache is shared across all visitors                        |
| `private`    | Cache is per-user (identified by cookie/session)           |
| `max-age=N`  | Cache lifetime in seconds                                  |
| `no-cache`   | Do not cache this response                                 |
| `no-vary`    | Ignore Vary header for caching (use carefully)             |

### X-LiteSpeed-Tag

Tags let you group cached pages so you can purge them selectively.

```
X-LiteSpeed-Tag: product:42, category:electronics, homepage
```

You can assign multiple comma-separated tags. When you purge by tag, all pages with that tag are invalidated.

### X-LiteSpeed-Purge

Sent in a response to trigger cache invalidation.

```
X-LiteSpeed-Purge: *                          # Purge everything
X-LiteSpeed-Purge: tag=product:42             # Purge by tag
X-LiteSpeed-Purge: tag=product:42,homepage    # Purge multiple tags
X-LiteSpeed-Purge: /about-us                  # Purge specific URL
```

## Public vs Private Cache

**Public cache** stores one copy of a page shared by all visitors:

```php
// Everyone sees the same homepage
header('X-LiteSpeed-Cache-Control: public,max-age=3600');
```

Use public cache for: landing pages, blog posts, product listings, static pages.

**Private cache** stores a separate copy per user (identified by a session cookie):

```php
// Each logged-in user has their own cached dashboard
header('X-LiteSpeed-Cache-Control: private,max-age=600');
```

Use private cache for: user dashboards, shopping carts, account pages.

**Example -- an e-commerce site:**

```php
if (!isset($_SESSION['user_id'])) {
    // Visitors all see the same product page
    header('X-LiteSpeed-Cache-Control: public,max-age=86400');
} else {
    // Logged-in users see personalized prices
    header('X-LiteSpeed-Cache-Control: private,max-age=1800');
}
```

## Cache Purge Methods

### Method 1: PURGE HTTP Request

Send a PURGE request to invalidate a specific URL:

```bash
curl -X PURGE https://example.com/about-us
```

### Method 2: Tag-Based Purge

Purge all pages with a specific tag by sending the header in any response:

```php
header('X-LiteSpeed-Purge: tag=blog');
```

### Method 3: Purge All

Nuclear option -- clears the entire cache:

```php
header('X-LiteSpeed-Purge: *');
```

Or from the command line:

```bash
curl -X PURGE -H "X-LiteSpeed-Purge: *" https://example.com/
```

## Cache Crawler / Cache Warming

LSCache supports a crawler that visits pages proactively so they are cached before real users arrive. The crawler identifies itself with a special header:

```
X-LSCACHE: on,crawler
```

When OLS sees this header from the crawler, it knows to cache the response even if no real user has visited the page yet.

In the WordPress LiteSpeed Cache plugin, configure the crawler under **LiteSpeed Cache > Crawler**:

- **Crawler**: ON
- **Crawl Interval**: 302 (seconds between crawl runs)
- **Run Duration**: 400 (seconds per crawl session)
- **Threads**: 3

The crawler will systematically visit your sitemap URLs, ensuring pages are cached and warm.

## Monitoring Cache Hits

Check whether a page is being served from cache:

```bash
curl -s -o /dev/null -D - https://example.com/ 2>&1 | grep -i litespeed
```

The `X-LiteSpeed-Cache-Status` header tells you:

| Value   | Meaning                                      |
|---------|----------------------------------------------|
| `hit`   | Served from cache, PHP was not invoked        |
| `miss`  | Not in cache, PHP generated the page          |
| `updating` | Stale cache served while background refresh happens |

If you consistently see `miss`, check the troubleshooting section below.

## Cache Configuration Options

These are set in the OLS configuration (httpd_config.conf or via the admin panel):

| Option          | Default (this build) | Description                                    |
|-----------------|----------------------|------------------------------------------------|
| `maxObjSize`    | 256MB                | Largest single object that can be cached        |
| `maxStale`      | 86400 (24h)          | Seconds stale content can be served             |
| `litemage`      | on                   | Enable LiteMage full-page cache for Magento     |
| `enableCache`   | 1                    | Master switch for LSCache                       |
| `expireInSeconds` | 3600               | Default TTL when no header is sent              |
| `privateExpireInSeconds` | 1800        | Default private cache TTL                       |
| `checkPublicCache` | 1                 | Check public cache for responses                |
| `checkPrivateCache` | 1                | Check private cache for responses               |

## Troubleshooting Cache Misses

**Problem: Every request is a "miss"**

- **Check if cache is enabled:** Verify `enableCache 1` in your OLS virtual host config.
- **Check for `no-cache` headers:** Your application may be sending `Cache-Control: no-cache` or `Pragma: no-cache`. These override LSCache.
- **Check for Set-Cookie:** Responses with `Set-Cookie` headers are not cached by default. Ensure your app does not set cookies on every page load.
- **Check Vary header:** A `Vary: Cookie` header combined with unique session cookies means every visitor gets a different cache entry. Use `no-vary` if appropriate.

**Problem: Cache works but pages show stale content**

- **Purge after updates:** Make sure your application sends `X-LiteSpeed-Purge` headers when content changes.
- **Reduce TTL:** Lower `max-age` for frequently updated pages.
- **Use tags:** Tag pages by content type so you can purge selectively.

**Problem: Logged-in users see cached public pages**

- **Use private cache or no-cache for authenticated users.**
- In WordPress, ensure "Cache Logged-in Users" is OFF in the LiteSpeed Cache plugin settings.

**Problem: POST requests are being cached**

- LSCache only caches GET and HEAD requests by default. If you see POST responses being cached, check for misconfigured rewrite rules that convert POSTs to GETs.

**Problem: Large pages are not cached**

- Check the `maxObjSize` setting. This build defaults to 256MB, but if your page (with all inline assets) exceeds that, it will not be cached. Increase the value if needed.
