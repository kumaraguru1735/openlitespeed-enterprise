# Configuration Basics

A complete, beginner-friendly guide to configuring OpenLiteSpeed. Every example can be copy-pasted directly into your config files.

---

## Table of Contents

1. [Configuration File Locations](#configuration-file-locations)
2. [How OLS Config Syntax Works](#how-ols-config-syntax-works)
3. [WebAdmin Console](#webadmin-console)
4. [Directory Structure Explained](#directory-structure-explained)
5. [Creating Your First Virtual Host](#creating-your-first-virtual-host)
6. [Setting Up PHP (LSAPI)](#setting-up-php-lsapi)
7. [Adding SSL / HTTPS](#adding-ssl--https)
8. [Setting Up Listeners](#setting-up-listeners)

---

## Configuration File Locations

OpenLiteSpeed installs to `/usr/local/lsws/` by default. Here are the key config files:

| File | Purpose |
|------|---------|
| `conf/httpd_config.conf` | **Main server config** -- listeners, virtual hosts, tuning, modules |
| `conf/vhosts/<name>/vhconf.conf` | **Per-virtual-host config** -- document root, rewrites, contexts |
| `admin/conf/admin_config.conf` | **WebAdmin console config** -- admin listener, SSL settings |
| `conf/mime.properties` | MIME type mappings (file extension to content-type) |
| `conf/templates/*.conf` | Virtual host templates for bulk hosting |

All paths below are relative to `/usr/local/lsws/` unless stated otherwise.

---

## How OLS Config Syntax Works

OpenLiteSpeed uses a **plain-text configuration format** (not XML, not YAML). Here are the rules:

### Key-Value Pairs

Simple settings are a key followed by whitespace and then a value:

```
serverName              myserver.example.com
user                    nobody
group                   nobody
indexFiles              index.html, index.php
```

- Keys are case-sensitive.
- Values can contain spaces (no quoting needed for most values).
- Comments start with `#`.

### Blocks

Related settings are grouped inside curly braces `{}`. The block name comes first:

```
errorlog logs/error.log {
    logLevel        DEBUG
    debugLevel      0
    rollingSize     10M
}
```

Some blocks have a type and name:

```
virtualHost Example {
    vhRoot          /usr/local/lsws/Example/
    configFile      conf/vhosts/Example/vhconf.conf
}
```

### Nested Blocks

Blocks can be nested:

```
tuning {
    maxConnections          10000
    connTimeout             300
    keepAliveTimeout        5
}
```

### Variables

OLS supports built-in variables in config values:

| Variable | Meaning |
|----------|---------|
| `$SERVER_ROOT` | Server installation directory (`/usr/local/lsws`) |
| `$VH_ROOT` | Virtual host root directory |
| `$VH_NAME` | Virtual host name |
| `$DOC_ROOT` | Document root of the current virtual host |

Example:

```
docRoot                 $VH_ROOT/html/
errorlog                $VH_ROOT/logs/error.log {
    logLevel            DEBUG
}
```

### Includes

You can split config into multiple files. The `configFile` directive inside a `virtualHost` block is the most common example:

```
virtualHost MySite {
    vhRoot              /var/www/mysite/
    configFile          conf/vhosts/MySite/vhconf.conf
}
```

The file at `conf/vhosts/MySite/vhconf.conf` contains all settings specific to that virtual host.

---

## WebAdmin Console

The WebAdmin console is a browser-based GUI for managing OpenLiteSpeed. It runs on **port 7080** by default.

### Accessing the console

1. Open a browser and go to: `https://your-server-ip:7080`
2. You will see a certificate warning (the default cert is self-signed). Accept it.
3. Log in with the admin credentials.

### Setting or resetting the admin password

If you have not set a password yet, or forgot it:

```bash
sudo /usr/local/lsws/admin/misc/admpass.sh
```

Example session:

```
Please specify the user name of administrator.
This is the user name required to login the management web interface.

User name [admin]: admin

Please specify the administrator's password.
Password:
Retype password:

Administrator's username/password is updated successfully!
```

### WebAdmin console features

- **Server Configuration** -- edit `httpd_config.conf` settings through a GUI
- **Virtual Hosts** -- create, edit, and delete virtual hosts
- **Listeners** -- manage HTTP/HTTPS listeners
- **Modules** -- enable/disable modules like cache, ESI, mod_security
- **Real-Time Statistics** -- view live connections, bandwidth, request rates
- **Actions** -- graceful restart, reload config, view logs

Any changes made in the WebAdmin console are written back to the plain-text config files, so you can always edit config files directly instead.

---

## Directory Structure Explained

```
/usr/local/lsws/
  |-- bin/                   # Server binary and control script (lswsctrl)
  |-- conf/                  # All configuration files
  |   |-- httpd_config.conf  # Main server configuration
  |   |-- mime.properties    # MIME types
  |   |-- vhosts/            # Per-virtual-host configs
  |   |   |-- Example/
  |   |       |-- vhconf.conf
  |   |       |-- htpasswd
  |   |       |-- htgroup
  |   |-- templates/         # Virtual host templates
  |-- admin/                 # WebAdmin console files
  |   |-- conf/              # Admin console configuration
  |   |-- html/              # Admin console web files
  |   |-- misc/              # Admin utilities (admpass.sh)
  |-- logs/                  # Server log files
  |   |-- error.log
  |   |-- access.log
  |-- tmp/                   # Temporary files, swap
  |-- cachedata/             # LSCache data storage
  |-- Example/               # Default virtual host document root
  |   |-- html/              # Web-accessible files go here
  |   |-- cgi-bin/           # CGI scripts
  |   |-- logs/              # Per-vhost logs
  |-- fcgi-bin/              # PHP LSAPI binary (lsphp)
  |-- docs/                  # Server documentation
  |-- modules/               # Loadable modules (.so files)
```

---

## Creating Your First Virtual Host

Let's create a virtual host for `example.com` that serves files from `/var/www/example.com/html/`.

### Step 1 -- Create the directory structure

```bash
sudo mkdir -p /var/www/example.com/html
sudo mkdir -p /var/www/example.com/logs

# Create a test page
echo '<h1>Hello from example.com!</h1>' | sudo tee /var/www/example.com/html/index.html
```

### Step 2 -- Create the virtual host config file

```bash
sudo mkdir -p /usr/local/lsws/conf/vhosts/example.com
```

Create `/usr/local/lsws/conf/vhosts/example.com/vhconf.conf`:

```
# Document root for this virtual host
docRoot /var/www/example.com/html/

# Enable gzip compression
enableGzip 1

# Default index files
index {
    indexFiles          index.html, index.php
    autoIndex           0
    useServer           0
}

# Root context -- serves all files under the document root
context / {
    allowBrowse         1
    location            $DOC_ROOT/
    rewrite {
        RewriteFile     .htaccess
    }
}

# Error pages
errorPage 404 {
    url                 /error404.html
}

# Per-vhost error log
errorlog /var/www/example.com/logs/error.log {
    logLevel            DEBUG
    rollingSize         10M
    useServer           1
}

# Per-vhost access log
accessLog /var/www/example.com/logs/access.log {
    rollingSize         10M
    keepDays            30
    compressArchive     0
    logReferer          1
    logUserAgent        1
    useServer           0
}

# Rewrite rules (disabled by default)
rewrite {
    enable              0
    logLevel            0
}

# Access control
accessControl {
    deny
    allow               *
}
```

### Step 3 -- Register the virtual host in the main config

Add this to `/usr/local/lsws/conf/httpd_config.conf`:

```
virtualHost example.com {
    vhRoot              /var/www/example.com/
    allowSymbolLink     1
    enableScript        1
    restrained          1
    setUIDMode          0
    configFile          conf/vhosts/example.com/vhconf.conf
}
```

### Step 4 -- Map the virtual host to a listener

Find your existing `listener` block and add a `map` line:

```
listener Default {
    address             *:8088
    secure              0
    map                 example.com     example.com
}
```

The format is `map <virtual-host-name> <comma-separated-domains>`. You can map multiple domains:

```
    map                 example.com     example.com, www.example.com
```

### Step 5 -- Restart the server

```bash
sudo /usr/local/lsws/bin/lswsctrl restart
```

### Step 6 -- Test

```bash
curl -H "Host: example.com" http://127.0.0.1:8088
# Should output: <h1>Hello from example.com!</h1>
```

---

## Setting Up PHP (LSAPI)

OpenLiteSpeed uses **LSAPI** (LiteSpeed Server API) to run PHP, which is significantly faster than PHP-FPM or mod_php. Here is how to set it up.

### Step 1 -- Install LSPHP

On Ubuntu/Debian:

```bash
sudo apt install -y lsphp83 lsphp83-common lsphp83-mysql lsphp83-curl
```

On CentOS/RHEL/AlmaLinux:

```bash
sudo dnf install -y lsphp83 lsphp83-common lsphp83-mysqlnd lsphp83-curl
```

The binary will be at `/usr/local/lsws/lsphp83/bin/lsphp`.

### Step 2 -- Define the external processor

Add this to `httpd_config.conf` (or edit the existing `lsphp` block):

```
extProcessor lsphp83 {
    type                    lsapi
    address                 uds://tmp/lshttpd/lsphp83.sock
    maxConns                10
    env                     PHP_LSAPI_CHILDREN=10
    env                     LSAPI_AVOID_FORK=200M
    initTimeout             60
    retryTimeout            0
    persistConn             1
    respBuffer              0
    autoStart               1
    path                    /usr/local/lsws/lsphp83/bin/lsphp
    backlog                 100
    instances               1
    priority                0
    memSoftLimit            2047M
    memHardLimit            2047M
    procSoftLimit           1400
    procHardLimit           1500
}
```

### Step 3 -- Map .php files to the handler

Add this to `httpd_config.conf`:

```
scriptHandler {
    add lsapi:lsphp83   php
}
```

This tells OpenLiteSpeed: "For any file ending in `.php`, send it to the `lsphp83` external processor."

### Step 4 -- Restart and test

```bash
sudo /usr/local/lsws/bin/lswsctrl restart
```

Create a test PHP file:

```bash
echo '<?php phpinfo(); ?>' | sudo tee /var/www/example.com/html/info.php
```

Visit `http://your-server-ip:8088/info.php` -- you should see the PHP info page.

**Important:** Delete the test file when done:

```bash
sudo rm /var/www/example.com/html/info.php
```

---

## Adding SSL / HTTPS

### Option A -- Using Let's Encrypt (recommended for production)

#### Step 1 -- Install certbot

```bash
# Ubuntu/Debian
sudo apt install -y certbot

# CentOS/RHEL/AlmaLinux
sudo dnf install -y certbot
```

#### Step 2 -- Obtain a certificate

Stop any server on port 80 temporarily, or use webroot mode:

```bash
# Standalone mode (stop OLS first)
sudo /usr/local/lsws/bin/lswsctrl stop
sudo certbot certonly --standalone -d example.com -d www.example.com
sudo /usr/local/lsws/bin/lswsctrl start

# Or webroot mode (no downtime)
sudo certbot certonly --webroot -w /var/www/example.com/html/ \
  -d example.com -d www.example.com
```

Certificates are saved to `/etc/letsencrypt/live/example.com/`.

#### Step 3 -- Configure the HTTPS listener

Add to `httpd_config.conf`:

```
listener HTTPS {
    address                 *:443
    secure                  1
    keyFile                 /etc/letsencrypt/live/example.com/privkey.pem
    certFile                /etc/letsencrypt/live/example.com/fullchain.pem
    map                     example.com     example.com, www.example.com
}
```

#### Step 4 -- Restart

```bash
sudo /usr/local/lsws/bin/lswsctrl restart
```

#### Step 5 -- Set up auto-renewal

```bash
sudo crontab -e
```

Add this line:

```
0 3 * * * certbot renew --quiet --deploy-hook "/usr/local/lsws/bin/lswsctrl restart"
```

### Option B -- Using a self-signed certificate (for testing)

```bash
sudo mkdir -p /usr/local/lsws/conf/ssl

sudo openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout /usr/local/lsws/conf/ssl/server.key \
  -out /usr/local/lsws/conf/ssl/server.crt \
  -subj "/CN=example.com"
```

Then configure the listener:

```
listener HTTPS {
    address                 *:443
    secure                  1
    keyFile                 /usr/local/lsws/conf/ssl/server.key
    certFile                /usr/local/lsws/conf/ssl/server.crt
    map                     example.com     example.com
}
```

---

## Setting Up Listeners

Listeners define which ports and IP addresses OpenLiteSpeed accepts connections on.

### HTTP listener on port 80

```
listener HTTP {
    address                 *:80
    secure                  0
    map                     example.com     example.com, www.example.com
}
```

- `address *:80` means listen on all IP addresses, port 80.
- `secure 0` means this is a plain HTTP listener (no SSL).
- `map` connects a virtual host to this listener for specific domain names.

### HTTPS listener on port 443

```
listener HTTPS {
    address                 *:443
    secure                  1
    keyFile                 /etc/letsencrypt/live/example.com/privkey.pem
    certFile                /etc/letsencrypt/live/example.com/fullchain.pem
    certChain               1
    sslProtocol             24
    map                     example.com     example.com, www.example.com
}
```

- `secure 1` enables SSL/TLS.
- `sslProtocol 24` enables TLS 1.2 and TLS 1.3 (recommended).
- `certChain 1` tells OLS the cert file includes the full chain.

### Binding to a specific IP

If your server has multiple IPs:

```
listener SiteA {
    address                 192.168.1.10:80
    secure                  0
    map                     siteA     siteA.com
}

listener SiteB {
    address                 192.168.1.11:80
    secure                  0
    map                     siteB     siteB.com
}
```

### Mapping multiple virtual hosts to one listener

A single listener can serve many virtual hosts. OLS uses the `Host` header to route requests:

```
listener Default {
    address                 *:80
    secure                  0
    map                     site1     site1.com, www.site1.com
    map                     site2     site2.com, www.site2.com
    map                     site3     site3.com
}
```

### Complete minimal working configuration

Here is a full `httpd_config.conf` that serves one site with PHP:

```
serverName                  myserver.example.com
user                        nobody
group                       nobody
priority                    0
autoRestart                 1
inMemBufSize                60M
swappingDir                 /tmp/lshttpd/swap
indexFiles                  index.html, index.php

errorlog logs/error.log {
    logLevel                DEBUG
    debugLevel              0
    rollingSize             10M
    enableStderrLog         1
}

accessLog logs/access.log {
    rollingSize             10M
    keepDays                30
    compressArchive         0
}

tuning {
    maxConnections          10000
    maxSSLConnections       10000
    connTimeout             300
    maxKeepAliveReq         10000
    keepAliveTimeout        5
    enableGzipCompress      1
    enableBrCompress        4
    enableDynGzipCompress   1
    gzipCompressLevel       6
    compressibleTypes       default
}

accessControl {
    allow                   ALL
    deny
}

extProcessor lsphp83 {
    type                    lsapi
    address                 uds://tmp/lshttpd/lsphp83.sock
    maxConns                10
    env                     PHP_LSAPI_CHILDREN=10
    env                     LSAPI_AVOID_FORK=200M
    initTimeout             60
    retryTimeout            0
    persistConn             1
    respBuffer              0
    autoStart               1
    path                    /usr/local/lsws/lsphp83/bin/lsphp
    backlog                 100
    instances               1
}

scriptHandler {
    add lsapi:lsphp83      php
}

virtualHost example.com {
    vhRoot                  /var/www/example.com/
    allowSymbolLink         1
    enableScript            1
    restrained              1
    configFile              conf/vhosts/example.com/vhconf.conf
}

listener HTTP {
    address                 *:80
    secure                  0
    map                     example.com     example.com, www.example.com
}

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

After editing any config file, always restart:

```bash
sudo /usr/local/lsws/bin/lswsctrl restart
```

Check the error log if anything goes wrong:

```bash
sudo tail -30 /usr/local/lsws/logs/error.log
```
