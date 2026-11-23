#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <tor_api.h>

// ============================================================================
// PART 1: Thread-Safe Recycling Free-List Allocator (Single 256 MB VMA)
// ============================================================================
#define ARENA_SIZE (256 * 1024 * 1024)
#define NUM_BINS   32
#define BLOCK_MAGIC_USED 0x544F525553454421ULL
#define BLOCK_MAGIC_FREE 0x544F524652454521ULL

typedef struct block_hdr {
    size_t size;                 // Payload size (16-byte aligned)
    size_t magic;                // USED or FREE magic
    struct block_hdr *prev_phys; // Previous adjacent block in memory
    struct block_hdr *next_free; // Next free block in bin
    struct block_hdr *prev_free; // Prev free block in bin
} block_hdr_t;

static uint8_t *arena_base = NULL;
static size_t arena_offset = 0;
static block_hdr_t *last_phys_block = NULL;
static block_hdr_t *free_bins[NUM_BINS] = { NULL };
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static inline int size_to_bin(size_t size) {
    if (size <= 32) return 0;
    int bin = 0;
    size_t s = (size - 1) >> 5;
    while (s > 0 && bin < NUM_BINS - 1) {
        s >>= 1;
        bin++;
    }
    return bin;
}

static inline void bin_insert(block_hdr_t *blk) {
    int bin = size_to_bin(blk->size);
    blk->magic = BLOCK_MAGIC_FREE;
    blk->prev_free = NULL;
    blk->next_free = free_bins[bin];
    if (free_bins[bin]) {
        free_bins[bin]->prev_free = blk;
    }
    free_bins[bin] = blk;
}

static inline void bin_remove(block_hdr_t *blk) {
    int bin = size_to_bin(blk->size);
    if (blk->prev_free) {
        blk->prev_free->next_free = blk->next_free;
    } else {
        free_bins[bin] = blk->next_free;
    }
    if (blk->next_free) {
        blk->next_free->prev_free = blk->prev_free;
    }
    blk->prev_free = NULL;
    blk->next_free = NULL;
}

static inline block_hdr_t *next_phys_block(block_hdr_t *blk) {
    uint8_t *next_addr = (uint8_t *)(blk + 1) + blk->size;
    if (next_addr >= arena_base + arena_offset) return NULL;
    return (block_hdr_t *)next_addr;
}

void *malloc(size_t size) {
    if (size == 0) size = 16;
    size = (size + 15) & ~((size_t)15);

    pthread_mutex_lock(&alloc_lock);

    if (!arena_base) {
        arena_base = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (arena_base == MAP_FAILED) {
            arena_base = NULL;
            pthread_mutex_unlock(&alloc_lock);
            return NULL;
        }
    }

    int start_bin = size_to_bin(size);
    for (int b = start_bin; b < NUM_BINS; b++) {
        block_hdr_t *curr = free_bins[b];
        while (curr) {
            if (curr->size >= size) {
                bin_remove(curr);

                size_t min_split = sizeof(block_hdr_t) + 32;
                if (curr->size >= size + min_split) {
                    block_hdr_t *rem = (block_hdr_t *)((uint8_t *)(curr + 1) + size);
                    rem->size = curr->size - size - sizeof(block_hdr_t);
                    rem->prev_phys = curr;
                    curr->size = size;

                    block_hdr_t *after = next_phys_block(rem);
                    if (after) {
                        after->prev_phys = rem;
                    } else {
                        last_phys_block = rem;
                    }
                    bin_insert(rem);
                }

                curr->magic = BLOCK_MAGIC_USED;
                pthread_mutex_unlock(&alloc_lock);
                return (void *)(curr + 1);
            }
            curr = curr->next_free;
        }
    }

    size_t total = sizeof(block_hdr_t) + size;
    if (arena_offset + total <= ARENA_SIZE) {
        block_hdr_t *blk = (block_hdr_t *)(arena_base + arena_offset);
        arena_offset += total;
        blk->size = size;
        blk->magic = BLOCK_MAGIC_USED;
        blk->prev_phys = last_phys_block;
        blk->next_free = NULL;
        blk->prev_free = NULL;
        last_phys_block = blk;

        pthread_mutex_unlock(&alloc_lock);
        return (void *)(blk + 1);
    }

    pthread_mutex_unlock(&alloc_lock);
    return NULL;
}

void free(void *ptr) {
    if (!ptr) return;

    pthread_mutex_lock(&alloc_lock);

    block_hdr_t *blk = ((block_hdr_t *)ptr) - 1;
    if (blk->magic != BLOCK_MAGIC_USED) {
        pthread_mutex_unlock(&alloc_lock);
        return;
    }

    block_hdr_t *next = next_phys_block(blk);
    if (next && next->magic == BLOCK_MAGIC_FREE) {
        bin_remove(next);
        blk->size += sizeof(block_hdr_t) + next->size;
        block_hdr_t *after = next_phys_block(blk);
        if (after) {
            after->prev_phys = blk;
        } else {
            last_phys_block = blk;
        }
    }

    block_hdr_t *prev = blk->prev_phys;
    if (prev && prev->magic == BLOCK_MAGIC_FREE) {
        bin_remove(prev);
        prev->size += sizeof(block_hdr_t) + blk->size;
        block_hdr_t *after = next_phys_block(prev);
        if (after) {
            after->prev_phys = prev;
        } else {
            last_phys_block = prev;
        }
        blk = prev;
    }

    bin_insert(blk);
    pthread_mutex_unlock(&alloc_lock);
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
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    block_hdr_t *blk = ((block_hdr_t *)ptr) - 1;
    if (blk->magic != BLOCK_MAGIC_USED) return malloc(size);
    if (blk->size >= size) return ptr;

    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, blk->size);
    free(ptr);
    return new_ptr;
}
// ============================================================================
// PART 2: PS4 Stubs, Notifications & Tor Daemon Control
// ============================================================================
struct group *getgrgid(gid_t gid) { (void)gid; return NULL; }
int setgid(gid_t gid) { (void)gid; return 0; }

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
