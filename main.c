#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <tor_api.h>

// O(1) Power-of-2 recycling slab allocator inside a single 256 MB VMA
// Uses sched_yield() to prevent Orbis OS thread starvation at 95% bootstrap
// and recycles slots forever to prevent VMA exhaustion during hidden service / circuit churn
#define ARENA_SIZE (256 * 1024 * 1024)
#define NUM_BINS   28
#define ALLOC_MAGIC_USED 0x544F524D454D4F52ULL
#define ALLOC_MAGIC_FREE 0x544F524652454521ULL

typedef struct alloc_hdr {
    size_t bin;
    size_t magic;
    struct alloc_hdr *next_free;
} alloc_hdr_t;

static uint8_t *arena_base = NULL;
static size_t arena_offset = 0;
static alloc_hdr_t *free_bins[NUM_BINS] = { NULL };
static volatile int alloc_lock = 0;

static inline void lock_alloc(void) {
    while (__atomic_test_and_set(&alloc_lock, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
}

static inline void unlock_alloc(void) {
    __atomic_clear(&alloc_lock, __ATOMIC_RELEASE);
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

static void *malloc_internal(size_t size) {
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

    alloc_hdr_t *hdr = free_bins[bin];
    if (hdr) {
        free_bins[bin] = hdr->next_free;
        hdr->next_free = NULL;
        hdr->magic = ALLOC_MAGIC_USED;
        return (void *)(hdr + 1);
    }

    size_t total = sizeof(alloc_hdr_t) + slot_size;
    if (arena_offset + total <= ARENA_SIZE) {
        hdr = (alloc_hdr_t *)(arena_base + arena_offset);
        arena_offset += total;
        hdr->bin = bin;
        hdr->magic = ALLOC_MAGIC_USED;
        hdr->next_free = NULL;
        return (void *)(hdr + 1);
    }

    return NULL;
}

static void free_internal(void *ptr) {
    if (!ptr) return;
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    if (hdr->magic == ALLOC_MAGIC_USED && hdr->bin < NUM_BINS) {
        hdr->magic = ALLOC_MAGIC_FREE;
        hdr->next_free = free_bins[hdr->bin];
        free_bins[hdr->bin] = hdr;
    }
}

void *malloc(size_t size) {
    lock_alloc();
    void *ptr = malloc_internal(size);
    unlock_alloc();
    return ptr;
}

void free(void *ptr) {
    if (!ptr) return;
    lock_alloc();
    free_internal(ptr);
    unlock_alloc();
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;
    size_t total = nmemb * size;
    lock_alloc();
    void *ptr = malloc_internal(total);
    unlock_alloc();
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    lock_alloc();
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != ALLOC_MAGIC_USED || hdr->bin >= NUM_BINS) {
        void *p = malloc_internal(size);
        unlock_alloc();
        return p;
    }

    size_t old_size = bin_to_size(hdr->bin);
    if (old_size >= size) {
        unlock_alloc();
        return ptr;
    }

    void *new_ptr = malloc_internal(size);
    if (!new_ptr) {
        unlock_alloc();
        return NULL;
    }
    memcpy(new_ptr, ptr, old_size);
    free_internal(ptr);
    unlock_alloc();
    return new_ptr;
}

// Stubs for Unix group functions missing from PS4 libc
struct group *getgrgid(gid_t gid) { (void)gid; return NULL; }
int setgid(gid_t gid) { (void)gid; return 0; }

// PS4 system notification structure
typedef struct {
    char useless1[45];
    char message[1024];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

static void ps4_notify(const char *msg) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message, msg, sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
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
            "# Use any available local port starting from 8081 (port 8080 is reserved for webtor-ps4):\n"
            "# HiddenServiceDir /data/tor/data/hidden_service/\n"
            "# HiddenServicePort 80 127.0.0.1:8081\n"
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
