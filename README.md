# PS4 Tor Daemon Payload (`ps4-tor`)

A standalone native C-Tor background daemon payload (`.elf`) for jailbroken PlayStation 4 consoles. Runs in the background and exposes local **SOCKS5** and **HTTP Tunnel** proxies on `127.0.0.1`.

## Features
- Native C-Tor compiled with OpenSSL, libevent, and zlib for Orbis OS (`x86_64-pc-freebsd9`).
- **SOCKS5 Proxy:** `127.0.0.1:9050` (for homebrew apps / `libcurl` with `socks5h://` — works automatically without manual proxy configuration).
- **HTTP Tunnel Proxy:** `127.0.0.1:8118` (for tunneling PS4 System Network traffic via Proxy settings).
- Auto-generates `/data/tor/torrc` on first run while preserving custom edits.
- Custom `mmap`-backed memory allocator to bypass GoldHEN's static heap limit.

## Usage on PS4
1. Enable **FTP Server** in **GoldHEN → Settings → Server Settings** (port `2121`).
2. Upload `tor-ps4.elf` via FTP to **`/data/payloads/tor-ps4.elf`** (or **`/data/GoldHEN/payloads/tor-ps4.elf`**).
3. On your PS4, open **GoldHEN → Utilities → PayLoader → LaunchPad** and select **`tor-ps4.elf`**.
4. Wait ~15–20 seconds for initial bootstrap and check the log via `curl` (replace `<PS4_IP>` with your console's IP address) until you see `Bootstrapped 100%`:
   ```bash
   curl -s ftp://<PS4_IP>:2121/data/tor/tor.log
   ```

### Network Setup

- **For Homebrew Apps (Default / No Proxy Needed):**
  Just use **Easy** setup in **PS4 Settings → Network → Set Up Internet Connection**. You do not need to enter an IP address or port anywhere—compatible homebrew apps will automatically use Tor on `127.0.0.1:9050`.

- **For PS4 System-Wide HTTP Tunneling (Optional):**
  If you want to tunnel general PS4 system traffic through Tor, go to **PS4 Settings → Network → Set Up Internet Connection → Custom → Proxy Server → Use** and enter:
  - **Address:** `127.0.0.1`
  - **Port:** `8118`

  > **Note on PS4 Browser Limitations:** The built-in PS4 web browser lacks modern SSL/TLS support and blocks `.onion` domains, even when tunneled through port `8118`. To browse `.onion` sites on the PS4 browser, we are working on a dedicated companion web proxy payload: **[webtor-ps4](https://github.com/jessyclement2026/webtor-ps4)** *(WIP)*.

## Building from Source

### 1. Install Build Tools & SDK Dependencies

- **Arch / CachyOS:**
  ```bash
  sudo pacman -S --needed base-devel clang lld llvm cmake autoconf automake libtool pkgconf git wget python
  ```

- **Debian / Ubuntu:**
  ```bash
  sudo apt-get update && sudo apt-get upgrade # optional
  sudo apt-get install -y bash clang-18 lld-18 build-essential autoconf automake libtool git wget python3 # required
  sudo apt-get install -y socat cmake meson pkg-config # optional / build tools
  ```

- **Fedora (tested on Fedora 41):**
  ```bash
  sudo dnf install -y bash llvm-devel clang lld autoconf automake libtool git wget python3 # required
  sudo dnf install -y socat cmake meson pkg-config # optional / build tools
  ```

- **macOS (tested on macOS Sequoia via Homebrew):**
  ```bash
  brew install llvm@18 autoconf automake libtool wget # required
  export LLVM_CONFIG=/opt/homebrew/opt/llvm@18/bin/llvm-config # required
  brew install socat cmake meson pkg-config # optional / build tools
  ```

### 2. Download `ps4-payload-sdk` & Build Payload
```bash
wget https://github.com/jessyclement2026/webtor-ps4/ps4-payload-dev/sdk/releases/latest/download/ps4-payload-sdk.zip
unzip ps4-payload-sdk.zip
export PS4SDK=\$PWD/ps4-payload-sdk

chmod +x build.sh
./build.sh
```
This compiles `zlib`, `OpenSSL`, `libevent`, `libtor.a`, and outputs `tor-ps4.elf`.

## Third-Party Libraries
Pre-compiled releases statically link:
- **Tor (`libtor`)** (BSD 3-Clause License)
- **OpenSSL** (Apache License 2.0)
- **libevent** (BSD 3-Clause License)
- **zlib** (zlib License)

## License & Disclaimer
This project is licensed under the **BSD 3-Clause License** — see the [LICENSE](LICENSE) file for details.  
*Note: This is an unofficial community port and is not affiliated with or endorsed by The Tor Project.*
