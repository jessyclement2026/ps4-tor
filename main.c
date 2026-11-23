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

// Custom 128 MB arena + mmap fallback allocator to prevent PS4 VMA exhaustion
#define ARENA_SIZE (128 * 1024 * 1024)
static uint8_t *arena_base = NULL;
static size_t arena_offset = 0;

typedef struct {
    size_t size;
    size_t magic;
} alloc_hdr_t;

#define ALLOC_ARENA_MAGIC 0x544F524152454E41ULL
#define ALLOC_MMAP_MAGIC  0x544F524D454D4F52ULL

void *malloc(size_t size) {
    if (size == 0) size = 1;
    size = (size + 15) & ~((size_t)15);
    size_t total = size + sizeof(alloc_hdr_t);

    if (!arena_base) {
        arena_base = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }

    if (arena_base != MAP_FAILED && (arena_offset + total) <= ARENA_SIZE) {
        alloc_hdr_t *hdr = (alloc_hdr_t *)(arena_base + arena_offset);
        arena_offset += total;
        hdr->size = size;
        hdr->magic = ALLOC_ARENA_MAGIC;
        return (void *)(hdr + 1);
    }

    alloc_hdr_t *hdr = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hdr == MAP_FAILED) return NULL;
    hdr->size = size;
    hdr->magic = ALLOC_MMAP_MAGIC;
    return (void *)(hdr + 1);
}

void free(void *ptr) {
    if (!ptr) return;
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    if (hdr->magic == ALLOC_MMAP_MAGIC) {
        hdr->magic = 0;
        munmap(hdr, hdr->size + sizeof(alloc_hdr_t));
    } else if (hdr->magic == ALLOC_ARENA_MAGIC) {
        hdr->magic = 0;
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    alloc_hdr_t *hdr = ((alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != ALLOC_ARENA_MAGIC && hdr->magic != ALLOC_MMAP_MAGIC) return malloc(size);
    if (hdr->size >= size) return ptr;
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, hdr->size);
    free(ptr);
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
            "MaxMemInQueues 32 MB\n"
            "ConfluxEnabled 0\n"
            "AvoidDiskWrites 1\n"
            "Log notice file /data/tor/tor.log\n"
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
    ps4_notify("Starting Tor Proxy (SOCKS5: 9050 | HTTP: 8118)...");

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
