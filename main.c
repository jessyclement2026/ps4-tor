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

// ============================================================================
// PART 1: O(1) Power-of-2 Slab Allocator (Zero Scanning, Single 256 MB VMA)
// ============================================================================
#define ARENA_SIZE (256 * 1024 * 1024)
#define NUM_BINS   28
#define BLOCK_MAGIC_USED 0x544F525553454421ULL
#define BLOCK_MAGIC_FREE 0x544F524652454521ULL

typedef struct block_hdr {
    size_t bin;                  // Power-of-2 bin index (0 = 32B, 1 = 64B, ...)
    size_t magic;                // USED or FREE magic
    struct block_hdr *next_free; // Next free block in exact-size bin
} block_hdr_t;

static uint8_t *arena_base = NULL;
static size_t arena_offset = 0;
static block_hdr_t *free_bins[NUM_BINS] = { NULL };
static volatile int alloc_spinlock = 0;

static inline void lock_alloc(void) {
    while (__atomic_test_and_set(&alloc_spinlock, __ATOMIC_ACQUIRE)) {
        __builtin_ia32_pause();
    }
}

static inline void unlock_alloc(void) {
    __atomic_clear(&alloc_spinlock, __ATOMIC_RELEASE);
}

static inline size_t size_to_bin(size_t size) {
    size_t bin = 0;
    size_t cap = 32;
    while (cap < size && bin < NUM_BINS - 1) {
        cap <<= 1;
        bin++;
    }
    return bin;
}

static inline size_t bin_to_size(size_t bin) {
    return ((size_t)32) << bin;
}
// ============================================================================
// PART 2: O(1) Constant-Time Slab Allocation & Recycling
// ============================================================================
static void *malloc_unlocked(size_t size) {
    if (size == 0) size = 16;
    size_t bin = size_to_bin(size);
    size_t slot_size = bin_to_size(bin);

    if (!arena_base) {
        arena_base = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (arena_base == MAP_FAILED) {
            arena_base = NULL;
            return NULL;
        }
    }

    // Pop head of exact-size bin in O(1) — zero linked-list scanning
    block_hdr_t *blk = free_bins[bin];
    if (blk) {
        free_bins[bin] = blk->next_free;
        blk->next_free = NULL;
        blk->magic = BLOCK_MAGIC_USED;
        return (void *)(blk + 1);
    }

    // Carve new exact-size slot from fresh arena space
    size_t total = sizeof(block_hdr_t) + slot_size;
    if (arena_offset + total <= ARENA_SIZE) {
        blk = (block_hdr_t *)(arena_base + arena_offset);
        arena_offset += total;
        blk->bin = bin;
        blk->magic = BLOCK_MAGIC_USED;
        blk->next_free = NULL;
        return (void *)(blk + 1);
    }

    return NULL;
}

static void free_unlocked(void *ptr) {
    if (!ptr) return;
    block_hdr_t *blk = ((block_hdr_t *)ptr) - 1;
    if (blk->magic != BLOCK_MAGIC_USED || blk->bin >= NUM_BINS) return;

    // Push back to exact-size bin in O(1)
    blk->magic = BLOCK_MAGIC_FREE;
    blk->next_free = free_bins[blk->bin];
    free_bins[blk->bin] = blk;
}
// ============================================================================
// PART 3: Public Allocator Wrappers & PS4 Notification Helpers
// ============================================================================
void *malloc(size_t size) {
    lock_alloc();
    void *ptr = malloc_unlocked(size);
    unlock_alloc();
    return ptr;
}

void free(void *ptr) {
    if (!ptr) return;
    lock_alloc();
    free_unlocked(ptr);
    unlock_alloc();
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;
    size_t total = nmemb * size;
    lock_alloc();
    void *ptr = malloc_unlocked(total);
    unlock_alloc();
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    lock_alloc();
    block_hdr_t *blk = ((block_hdr_t *)ptr) - 1;
    if (blk->magic != BLOCK_MAGIC_USED || blk->bin >= NUM_BINS) {
        void *p = malloc_unlocked(size);
        unlock_alloc();
        return p;
    }

    size_t old_size = bin_to_size(blk->bin);
    if (old_size >= size) {
        unlock_alloc();
        return ptr;
    }

    void *new_ptr = malloc_unlocked(size);
    if (!new_ptr) {
        unlock_alloc();
        return NULL;
    }
    memcpy(new_ptr, ptr, old_size);
    free_unlocked(ptr);
    unlock_alloc();
    return new_ptr;
}

struct group *getgrgid(gid_t gid) { (void)gid; return NULL; }
int setgid(gid_t gid) { (void)gid; return 0; }

typedef struct {
    char useless1;
    char message;
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

static void ps4_notify(const char *msg) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
// ============================================================================
// PART 4: Tor Process Control, Default torrc & Entry Point
// ============================================================================
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

static int stop_running_tor(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9051);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return 0;
    }

    const char *cmd = "AUTHENTICATE \"\"\r\nSIGNAL SHUTDOWN\r\n";
    send(sock, cmd, strlen(cmd), 0);
    char *resp = calloc(1, 128);
    if (resp) {
        recv(sock, resp, 127, 0);
        free(resp);
    }
    close(sock);
    return 1;
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
            "VirtualAddrNetworkIPv4 127.192.0.0/10\n"
            "CookieAuthentication 0\n"
            "MaxMemInQueues 16 MB\n"
            "DirCache 0\n"
            "ClientOnly 1\n"
            "NumCPUs 1\n"
            "ConfluxEnabled 0\n"
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
            "# Use any available local port starting from 8081 (port 8080 is reserved for webtor-ps4):\n"
            "# HiddenServiceDir /data/tor/data/hidden_service/\n"
            "# HiddenServicePort 80 127.0.0.1:8081\n"
        );
        fclose(f);
    }
}

int main(void) {
    if (is_tor_running()) {
        if (stop_running_tor()) {
            ps4_notify("Tor Proxy stopped!");
        } else {
            ps4_notify("Tor is running (add ControlPort 127.0.0.1:9051 to torrc to enable stop toggle)");
        }
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
