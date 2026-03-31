# Installation Guide

A complete, beginner-friendly guide to installing OpenLiteSpeed from source.

---

## Table of Contents

1. [System Requirements](#system-requirements)
2. [Installing on Ubuntu / Debian](#installing-on-ubuntu--debian)
3. [Installing on CentOS / RHEL / AlmaLinux](#installing-on-centos--rhel--almalinux)
4. [Post-Install Setup](#post-install-setup)
5. [Replacing an Existing OpenLiteSpeed Installation](#replacing-an-existing-openlitespeed-installation)
6. [Setting Up a systemd Service](#setting-up-a-systemd-service)
7. [Verifying the Installation](#verifying-the-installation)
8. [Uninstalling](#uninstalling)

---

## System Requirements

| Resource | Minimum           | Recommended         |
|----------|-------------------|---------------------|
| CPU      | 1 core (x86_64)   | 2+ cores            |
| RAM      | 512 MB            | 2 GB+               |
| Disk     | 500 MB free       | 2 GB+ free          |
| OS       | Ubuntu 20.04+, Debian 11+, CentOS 7+, RHEL 8+, AlmaLinux 8+ | Ubuntu 22.04 LTS or AlmaLinux 9 |

OpenLiteSpeed runs on **Linux only** (x86_64 and aarch64). You need root or sudo access for all steps below.

---

## Installing on Ubuntu / Debian

### Step 1 -- Update the system

```bash
sudo apt update && sudo apt upgrade -y
```

### Step 2 -- Install build dependencies

These are the compiler toolchain and libraries that OpenLiteSpeed needs to compile:

```bash
sudo apt install -y \
  build-essential \
  cmake \
  git \
  libpcre3-dev \
  libexpat1-dev \
  libssl-dev \
  zlib1g-dev \
  libbrotli-dev \
  libgeoip-dev \
  libudns-dev \
  autoconf \
  automake \
  libtool \
  pkg-config \
  curl \
  wget
```

### Step 3 -- Clone the repository

```bash
cd /opt
sudo git clone https://github.com/litespeedtech/openlitespeed.git
cd openlitespeed
```

Or, if you already have the source in `/root/OLS`:

```bash
cd /root/OLS
```

### Step 4 -- Download required third-party libraries

OpenLiteSpeed bundles scripts to fetch BoringSSL and Brotli:

```bash
sudo ./dlbssl.sh        # Download and build BoringSSL
sudo ./dlbrotli.sh       # Download and build Brotli
```

### Step 5 -- Configure the build

Using the included `build.sh` script (easiest method):

```bash
sudo ./build.sh
```

Or manually with CMake:

```bash
mkdir build-dir && cd build-dir
cmake -DCMAKE_INSTALL_PREFIX=/usr/local/lsws ..
make -j$(nproc)
```

Or manually with autotools:

```bash
./configure --prefix=/usr/local/lsws
make -j$(nproc)
```

### Step 6 -- Install

```bash
sudo make install
```

This installs OpenLiteSpeed to `/usr/local/lsws/` by default.

### Step 7 -- Set the admin password

```bash
sudo /usr/local/lsws/admin/misc/admpass.sh
```

You will be prompted to enter an admin username (default: `admin`) and a password. Remember these -- you will use them to log into the WebAdmin console.

---

## Installing on CentOS / RHEL / AlmaLinux

### Step 1 -- Update the system

```bash
sudo dnf update -y          # RHEL 8+, AlmaLinux 8+
# or for CentOS 7:
# sudo yum update -y
```

### Step 2 -- Enable development tools and EPEL

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y epel-release
```

For CentOS 7:

```bash
sudo yum groupinstall -y "Development Tools"
sudo yum install -y epel-release
```

### Step 3 -- Install build dependencies

```bash
sudo dnf install -y \
  cmake \
  git \
  pcre-devel \
  expat-devel \
  openssl-devel \
  zlib-devel \
  brotli-devel \
  GeoIP-devel \
  udns-devel \
  autoconf \
  automake \
  libtool \
  pkgconfig \
  curl \
  wget
```

For CentOS 7, replace `dnf` with `yum` in the command above.

### Step 4 -- Clone and build

```bash
cd /opt
sudo git clone https://github.com/litespeedtech/openlitespeed.git
cd openlitespeed

sudo ./dlbssl.sh
sudo ./dlbrotli.sh

sudo ./build.sh
```

### Step 5 -- Install

```bash
sudo make install
```

### Step 6 -- Open firewall ports

```bash
# HTTP, HTTPS, and WebAdmin console
sudo firewall-cmd --permanent --add-port=80/tcp
sudo firewall-cmd --permanent --add-port=443/tcp
sudo firewall-cmd --permanent --add-port=7080/tcp
sudo firewall-cmd --reload
```

### Step 7 -- Set the admin password

```bash
sudo /usr/local/lsws/admin/misc/admpass.sh
```

---

## Post-Install Setup

### Directory permissions

The default installation user and group are `nobody:nobody`. If you want to run as a dedicated user:

```bash
# Create a dedicated user
sudo useradd -r -s /sbin/nologin lsadm

# Set ownership of the install directory
sudo chown -R lsadm:lsadm /usr/local/lsws

# Make sure log and tmp directories are writable
sudo chmod -R 755 /usr/local/lsws/logs
sudo chmod -R 755 /usr/local/lsws/tmp
```

Then update `user` and `group` in `/usr/local/lsws/conf/httpd_config.conf`:

```
user                lsadm
group               lsadm
```

### Create required directories

These should exist after install, but verify:

```bash
sudo mkdir -p /usr/local/lsws/logs
sudo mkdir -p /usr/local/lsws/tmp/swap
sudo mkdir -p /usr/local/lsws/cachedata
```

### Starting the server

```bash
sudo /usr/local/lsws/bin/lswsctrl start
```

Other control commands:

```bash
sudo /usr/local/lsws/bin/lswsctrl stop       # Stop the server
sudo /usr/local/lsws/bin/lswsctrl restart     # Restart the server
sudo /usr/local/lsws/bin/lswsctrl status      # Check if running
```

---

## Replacing an Existing OpenLiteSpeed Installation

If you already have OpenLiteSpeed installed (for example, from a package), follow these steps to replace it with a source build.

### Step 1 -- Stop the existing server

```bash
sudo /usr/local/lsws/bin/lswsctrl stop
# or if installed as a service:
sudo systemctl stop lsws
```

### Step 2 -- Back up your configuration

```bash
sudo cp -a /usr/local/lsws/conf /usr/local/lsws/conf.bak
sudo cp -a /usr/local/lsws/admin/conf /usr/local/lsws/admin/conf.bak
```

### Step 3 -- Build and install the new version

```bash
cd /root/OLS   # or wherever your source is
sudo ./build.sh
sudo make install
```

The installer will overwrite binaries but should preserve your existing configuration files. However, always keep the backup from Step 2.

### Step 4 -- Restore config if needed and restart

```bash
# If config was overwritten:
sudo cp -a /usr/local/lsws/conf.bak/* /usr/local/lsws/conf/

sudo /usr/local/lsws/bin/lswsctrl start
```

---

## Setting Up a systemd Service

Create a systemd unit file so OpenLiteSpeed starts automatically on boot.

### Step 1 -- Create the service file

```bash
sudo tee /etc/systemd/system/lsws.service > /dev/null << 'EOF'
[Unit]
Description=OpenLiteSpeed HTTP Server
After=network.target remote-fs.target nss-lookup.target

[Service]
Type=forking
PIDFile=/tmp/lshttpd/lshttpd.pid
ExecStartPre=/usr/local/lsws/bin/lswsctrl condmkdir
ExecStart=/usr/local/lsws/bin/lswsctrl start
ExecStop=/usr/local/lsws/bin/lswsctrl stop
ExecReload=/usr/local/lsws/bin/lswsctrl restart
Restart=on-failure
RestartSec=5
KillMode=process
PrivateTmp=false

# Security hardening (optional)
ProtectSystem=full
NoNewPrivileges=false
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF
```

### Step 2 -- Enable and start the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable lsws
sudo systemctl start lsws
```

### Step 3 -- Verify

```bash
sudo systemctl status lsws
```

You should see `active (running)` in the output.

Now the server will start automatically every time the system boots.

---

## Verifying the Installation

### Check that the server process is running

```bash
ps aux | grep litespeed
```

You should see one or more `litespeed` or `lshttpd` processes.

### Test with curl

```bash
# Test the default HTTP listener (port 8088 by default)
curl -I http://127.0.0.1:8088

# Expected output (something like):
# HTTP/1.1 200 OK
# Server: LiteSpeed
# ...
```

If you configured a listener on port 80:

```bash
curl -I http://127.0.0.1
```

### Test the WebAdmin console

```bash
curl -kI https://127.0.0.1:7080

# Expected output:
# HTTP/1.1 200 OK
# ...
```

Or open a browser and go to `https://your-server-ip:7080`. Log in with the admin credentials you set earlier.

### Check the error log

```bash
sudo tail -50 /usr/local/lsws/logs/error.log
```

Look for lines like:

```
[NOTICE] [ADMIN] server started successfully!
```

If you see errors, they will describe what went wrong (missing files, permission issues, port conflicts, etc.).

### Check the access log

```bash
sudo tail -20 /usr/local/lsws/logs/access.log
```

You should see your curl requests logged here.

---

## Uninstalling

### Step 1 -- Stop the server

```bash
sudo systemctl stop lsws 2>/dev/null
sudo /usr/local/lsws/bin/lswsctrl stop 2>/dev/null
```

### Step 2 -- Remove the systemd service (if you created one)

```bash
sudo systemctl disable lsws
sudo rm /etc/systemd/system/lsws.service
sudo systemctl daemon-reload
```

### Step 3 -- Remove the installation directory

```bash
sudo rm -rf /usr/local/lsws
```

### Step 4 -- Remove the dedicated user (if you created one)

```bash
sudo userdel lsadm
```

### Step 5 -- Clean up temporary files

```bash
sudo rm -rf /tmp/lshttpd
sudo rm -rf /dev/shm/ols*
```

That's it. OpenLiteSpeed is fully removed from the system.

---

## Quick Reference

| Task                  | Command                                        |
|-----------------------|------------------------------------------------|
| Start server          | `sudo /usr/local/lsws/bin/lswsctrl start`      |
| Stop server           | `sudo /usr/local/lsws/bin/lswsctrl stop`       |
| Restart server        | `sudo /usr/local/lsws/bin/lswsctrl restart`    |
| Check status          | `sudo /usr/local/lsws/bin/lswsctrl status`     |
| Set admin password    | `sudo /usr/local/lsws/admin/misc/admpass.sh`   |
| Main config file      | `/usr/local/lsws/conf/httpd_config.conf`       |
| Error log             | `/usr/local/lsws/logs/error.log`               |
| Access log            | `/usr/local/lsws/logs/access.log`              |
| WebAdmin console      | `https://your-server-ip:7080`                  |
