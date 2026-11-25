# PS4 Tor Daemon Payload (`ps4-tor`)

A standalone native C-Tor background daemon payload (`.elf`) for jailbroken PlayStation 4 consoles. Runs in the background and exposes local **SOCKS5** and **HTTP Tunnel** proxies on `127.0.0.1`.

## Features
- Native C-Tor compiled with OpenSSL, libevent, and zlib for Orbis OS (`x86_64-pc-freebsd9`).
- **SOCKS5 Proxy:** `127.0.0.1:9050` (for homebrew apps / `libcurl` with `socks5h://` — works automatically without manual proxy configuration).
- **HTTP Tunnel Proxy:** `127.0.0.1:8118` (for tunneling PS4 System Network traffic via Proxy settings).
- **Built-in Tor DNS & Sony Server Blocker:** `127.0.0.1:53` with `.onion` automapping (`127.192.0.0/10`) and automatic blocking of Sony/PSN update and telemetry domains. To prevent DNS leaks and keep Sony servers blocked when using the system proxy, set your PS4 network to **Custom** with both **Primary DNS** and **Secondary DNS** set to `127.0.0.1` (and Proxy Server set to `127.0.0.1`, Port `8118`).
- Auto-generates `/data/tor/torrc` on first run while preserving custom edits.
- Thread-safe recycling free-list memory allocator inside a single `mmap` VMA to bypass GoldHEN's static heap limit and prevent out-of-memory crashes.

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

### Hosting an `.onion` Hidden Service (Optional)
`tor-ps4` supports hosting Tor v3 `.onion` hidden services directly from your PS4:
1. Open **`/data/tor/torrc`** via FTP (port `2121`) and uncomment the two `HiddenService` lines at the bottom of the file (use any free local port starting from **`8081`**, since port `8080` is reserved for `webtor-ps4`):
   ```text
   HiddenServiceDir /data/tor/data/hidden_service/
   HiddenServicePort 80 127.0.0.1:8081
   ```
2. Point your local PS4 web server payload to listen on `127.0.0.1:8081`.
3. Restart `tor-ps4.elf` and retrieve your generated `.onion` address via FTP from **`/data/tor/data/hidden_service/hostname`**.

## Third-Party Libraries
Pre-compiled releases statically link:
- **Tor (`libtor`)** (BSD 3-Clause License)
- **OpenSSL** (Apache License 2.0)
- **libevent** (BSD 3-Clause License)
- **zlib** (zlib License)

## License & Disclaimer
This project is licensed under the **BSD 3-Clause License** — see the [LICENSE](LICENSE) file for details.  
*Note: This is an unofficial community port and is not affiliated with or endorsed by The Tor Project.*

> **⚠️ Warning:** Make sure **System Software Updates** are disabled in **PS4 Settings → System → Automatic Downloads** before going online so you do not accidentally upgrade your firmware and lose your jailbreak.

## Brewology Verification
Official releases for this project are also published on the Brewology PS4 Homebrew Store by **`cutiefly`**.

*Disclaimer: This software is an independent, open-source community project developed using the open-source `ps4-payload-sdk` and is not affiliated with or endorsed by The Tor Project or Sony Interactive Entertainment. It is provided "as is", without warranty of any kind, strictly for lawful privacy, educational, and research purposes. Users are solely responsible for complying with their local laws when using this software.*

---

## Credits & License
- **Tor Core:** Built on top of the C-Tor source code from [The Tor Project](https://torproject.org) (BSD-3-Clause).
- **Dependencies:** Statically linked with [OpenSSL 4.1.0](https://www.openssl.org/), [libevent 2.1.13-stable](https://github.com/libevent/libevent), and [zlib 1.4](https://zlib.net).
- **SDK:** [ps4-payload-dev / ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk).
- **License:** **GPL-3.0-or-later** (inherited from `PBS-Payload`).

---

## Support / Donations

If **ps4-stream** helped you stream your PS4 with ultra-low latency and you'd like to support further PS4 homebrew development (such as PS4 audio streaming support and testing on more firmwares):

- **Bitcoin (BTC):** `bc1qt9wxjtq6th3nxmkwg5pa8jhskrn29z6zd7nvum`

Starring the repository (⭐) or sharing test results on other PS4 firmwares also helps a lot!
