#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <tor_api.h>

// Zero-bug, 100% lock-free single-VMA (512 MB) bump allocator for small/medium blocks
// + direct mmap/munmap for large buffers (> 64 KB) + strdup/strndup overrides.
#define ARENA_SIZE      (512ULL * 1024ULL * 1024ULL)
#define LARGE_THRESHOLD (64ULL * 1024ULL)
#define MAGIC_ARENA     0x544F524152454E41ULL
#define MAGIC_MMAP      0x544F524D454D4F52ULL

typedef struct {
    size_t size;
    size_t magic;
} alloc_hdr_t;

static uint8_t *arena_base = NULL;
static size_t arena_offset = 0;

static inline uint8_t *get_arena(void) {
    uint8_t *base = __atomic_load_n(&arena_base, __ATOMIC_ACQUIRE);
    if (__builtin_expect(base != NULL, 1)) return base;

    uint8_t *mapped = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return NULL;

    uint8_t *expected = NULL;
    if (!__atomic_compare_exchange_n(&arena_base, &expected, mapped, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        munmap(mapped, ARENA_SIZE);
        return expected;
    }
    return mapped;
}

void *malloc(size_t size) {
    if (size == 0) size = 16;
    size = (size + 15) & ~((size_t)15);
    size_t total = size + sizeof(alloc_hdr_t);

    if (size <= LARGE_THRESHOLD) {
        uint8_t *base = get_arena();
        if (base) {
            size_t off = __atomic_fetch_add(&arena_offset, total, __ATOMIC_RELAXED);
            if (off + total <= ARENA_SIZE) {
                alloc_hdr_t *hdr = (alloc_hdr_t *)(base + off);
                hdr->size = size;
                hdr->magic = MAGIC_ARENA;
                return (void *)(hdr + 1);
            }
        }
    }

    alloc_hdr_t *hdr = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hdr == MAP_FAILED) return NULL;
    hdr->size = size;
    hdr->magic = MAGIC_MMAP;
    return (void *)(hdr + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    if (hdr->magic == MAGIC_MMAP) {
        hdr->magic = 0;
        munmap(hdr, hdr->size + sizeof(alloc_hdr_t));
    } else if (hdr->magic == MAGIC_ARENA) {
        hdr->magic = 0;
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != MAGIC_ARENA && hdr->magic != MAGIC_MMAP) return malloc(size);
    if (hdr->size >= size) return ptr;
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, hdr->size);
    free(ptr);
    return new_ptr;
}

char *strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

char *strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

// Stubs for Unix group functions missing from PS4 libc
struct group *getgrgid(gid_t gid) { (void)gid; return NULL; }
int setgid(gid_t gid) { (void)gid; return 0; }

// PS4 system notification structure
#define NOTIFY_PAD_LEN 45
#define NOTIFY_MSG_LEN 3075

typedef struct {
    char useless1[NOTIFY_PAD_LEN];
    char message[NOTIFY_MSG_LEN];
} notify_request_t;

int sceKernelDlsym(int handle, const char *symbol, void **addrp);

static void ps4_notify(const char *msg) {
    static int (*fn_notify)(int, notify_request_t *, size_t, int) = NULL;
    if (!fn_notify) {
        sceKernelDlsym(0x2001, "sceKernelSendNotificationRequest", (void **)&fn_notify);
    }
    if (!fn_notify) return;

    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    fn_notify(0, &req, sizeof(req), 0);
}

static int is_tor_running(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9050);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int res = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    return (res == 0);
}

static void ensure_tor_config(void) {
    mkdir("/data/tor", 0777);
    mkdir("/data/tor/data", 0700);

    // Only create default torrc if it doesn't exist so your edits stay intact
    FILE *f = fopen("/data/tor/torrc", "r");
    if (f) {
        fclose(f);
        return;
    }

    f = fopen("/data/tor/torrc", "w");
    if (f) {
        fprintf(f,
            "DataDirectory /data/tor/data\n"
            "SocksPort 127.0.0.1:9050\n"
            "HTTPTunnelPort 127.0.0.1:8118\n"
            "ControlPort 127.0.0.1:9051\n"
            "DNSPort 127.0.0.1:53\n"
            "AutomapHostsOnResolve 1\n"
            "VirtualAddrNetworkIPv4 198.18.0.0/15\n"
            "CookieAuthentication 0\n"
            "MaxMemInQueues 32 MB\n"
            "AvoidDiskWrites 1\n"
            "Log notice file /data/tor/tor.log\n"
            "\n"
            "# Block Sony / PlayStation servers (Firmware Updates & Telemetry)\n"
            "MapAddress *.playstation.com 0.0.0.0\n"
            "MapAddress playstation.com 0.0.0.0\n"
            "MapAddress *.playstation.net 0.0.0.0\n"
            "MapAddress playstation.net 0.0.0.0\n"
            "MapAddress *.psndl.net 0.0.0.0\n"
            "MapAddress psndl.net 0.0.0.0\n"
            "\n"
            "# Uncomment the lines below to host a .onion hidden service on your PS4\n"
            "# Use any available local port starting from 8080:\n"
            "# HiddenServiceDir /data/tor/data/hidden_service/\n"
            "# HiddenServicePort 80 127.0.0.1:8080\n"
        );
        fclose(f);
    }
}

int main(void) {
    if (is_tor_running()) {
        ps4_notify("Tor Proxy is already running on 127.0.0.1:9050");
        return 0;
    }

    ensure_tor_config();
    ps4_notify("Starting Tor Proxy (SOCKS5: 9050 | HTTP: 8118 | DNS: 53)...");

    tor_main_configuration_t *cfg = tor_main_configuration_new();
    char *argv[] = { "tor", "-f", "/data/tor/torrc" };

    if (tor_main_configuration_set_command_line(cfg, 3, argv) < 0) {
        ps4_notify("Tor config error! Check /data/tor/torrc");
        tor_main_configuration_free(cfg);
        return 1;
    }

    int ret = tor_run_main(cfg);
    tor_main_configuration_free(cfg);

    if (ret != 0) {
        ps4_notify("Tor exited with error! Check /data/tor/tor.log");
    }

    return ret;
}
