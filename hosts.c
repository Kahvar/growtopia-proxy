
#include "hosts.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define MARKER_LINE "# Proxy"
#define HOSTS_ENTRY_1 "127.0.0.1 growtopia1.com"
#define HOSTS_ENTRY_2 "127.0.0.1 www.growtopia1.com"

/* Compute path to the real System32 hosts file. If running 32-bit on 64-bit OS,
   use Sysnative to avoid Wow64 redirection. */
static void get_hosts_path(char* out, size_t out_size) {
    char windir[MAX_PATH];
    if (GetEnvironmentVariableA("windir", windir, MAX_PATH) == 0 || windir[0] == '\0') {
        strncpy(windir, "C:\\Windows", sizeof(windir) - 1);
        windir[sizeof(windir) - 1] = '\0';
    }

    BOOL is_wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &is_wow64) && is_wow64) {
        snprintf(out, out_size, "%s\\Sysnative\\drivers\\etc\\hosts", windir);
    } else {
        snprintf(out, out_size, "%s\\System32\\drivers\\etc\\hosts", windir);
    }
    out[out_size - 1] = '\0';
}

/* Open hosts file for direct reads; disables Wow64 redirection temporarily when needed. */
static FILE* open_hosts_file(const char* path, const char* mode) {
    BOOL is_wow64 = FALSE;
    PVOID old_value = NULL;
    FILE* file = NULL;

    if (IsWow64Process(GetCurrentProcess(), &is_wow64) && is_wow64) {
        if (Wow64DisableWow64FsRedirection(&old_value)) {
            file = fopen(path, mode);
            Wow64RevertWow64FsRedirection(old_value);
            return file;
        }
    }
    return fopen(path, mode);
}

static void log_host_resolution(const char* domain) {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "[HOSTS] WSAStartup failed\n");
        return;
    }

    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    struct addrinfo* result = NULL;
    int err = getaddrinfo(domain, NULL, &hints, &result);
    if (err != 0) {
        fprintf(stderr, "[HOSTS] getaddrinfo(%s) failed: %s\n", domain, gai_strerrorA(err));
        WSACleanup();
        return;
    }

    for (struct addrinfo* ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        char ipbuf[INET6_ADDRSTRLEN] = "";
        void* addr_ptr = NULL;
        if (ptr->ai_family == AF_INET) {
            addr_ptr = &((struct sockaddr_in*)ptr->ai_addr)->sin_addr;
        } else if (ptr->ai_family == AF_INET6) {
            addr_ptr = &((struct sockaddr_in6*)ptr->ai_addr)->sin6_addr;
        }
        if (addr_ptr && inet_ntop(ptr->ai_family, addr_ptr, ipbuf, sizeof(ipbuf))) {
            printf("[HOSTS] %s resolves to %s\n", domain, ipbuf);
        }
    }

    freeaddrinfo(result);
    WSACleanup();
}

/* Try to flush DNS via dnsapi.dll (DnsFlushResolverCache). If that fails, fallback to ipconfig. */
static void flush_dns_cache(void) {
    typedef DWORD (WINAPI *DNSFLUSHCACHE_FN)(void);
    HMODULE h = LoadLibraryA("dnsapi.dll");
    if (h) {
        DNSFLUSHCACHE_FN f = (DNSFLUSHCACHE_FN)GetProcAddress(h, "DnsFlushResolverCache");
        if (f) {
            (void)f();
            FreeLibrary(h);
            printf("[HOSTS] Flushed DNS resolver cache (via dnsapi.dll).\n");
            return;
        }
        FreeLibrary(h);
    }
    /* fallback */
    printf("[HOSTS] dnsapi.dll not usable; falling back to ipconfig /flushdns\n");
    system("cmd /C \"ipconfig /flushdns >nul 2>&1\"");
}

/* Rewrite the hosts file in place, preserving unrelated entries and removing any old proxy block.
   This avoids fragile command-line quoting and ensures the file is written from the same directory. */
static int write_hosts_file_with_block(const char* hosts_path) {
    char tmp_path[MAX_PATH];
    size_t hosts_len = strlen(hosts_path);
    if (hosts_len + 5 >= sizeof(tmp_path)) {
        fprintf(stderr, "[HOSTS] hosts path too long for temp path\n");
        return 1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", hosts_path);

    FILE* tmp_file = open_hosts_file(tmp_path, "wb");
    if (!tmp_file) {
        fprintf(stderr, "[HOSTS] Failed to open temporary hosts file: %s\n", tmp_path);
        return 1;
    }

    FILE* src = open_hosts_file(hosts_path, "rb");
    if (src) {
        char line[1024];
        while (fgets(line, sizeof(line), src)) {
            char* end = line + strlen(line);
            while (end > line && (end[-1] == '\r' || end[-1] == '\n')) {
                end[-1] = '\0';
                end--;
            }

            if (strcmp(line, MARKER_LINE) == 0 || strcmp(line, HOSTS_ENTRY_1) == 0 || strcmp(line, HOSTS_ENTRY_2) == 0) {
                continue;
            }

            fprintf(tmp_file, "%s\r\n", line);
        }
        fclose(src);
    }

    fprintf(tmp_file, "%s\r\n%s\r\n%s\r\n", MARKER_LINE, HOSTS_ENTRY_1, HOSTS_ENTRY_2);
    fclose(tmp_file);

    if (!MoveFileExA(tmp_path, hosts_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "[HOSTS] Failed to replace hosts file: %lu\n", GetLastError());
        remove(tmp_path);
        return 1;
    }

    return 0;
}

int add_hosts(void) {
    char hosts_path[MAX_PATH];
    get_hosts_path(hosts_path, sizeof(hosts_path));
    printf("[HOSTS] Using hosts file: %s\n", hosts_path);

    if (write_hosts_file_with_block(hosts_path) != 0) {
        return 1;
    }

    printf("[HOSTS] Redirect block written.\n");
    flush_dns_cache();
    log_host_resolution("www.growtopia1.com");
    log_host_resolution("growtopia1.com");
    return 0;
}

/* Remove the proxy block by writing a filtered tmp file in the same directory and moving it atomically. */
int remove_hosts(void) {
    char hosts_path[MAX_PATH];
    get_hosts_path(hosts_path, sizeof(hosts_path));
    printf("[HOSTS] Removing proxy block from hosts file: %s\n", hosts_path);

    FILE* file = open_hosts_file(hosts_path, "rb");
    if (!file) {
        printf("[HOSTS] Hosts file does not exist; nothing to remove.\n");
        return 0;
    }

    char tmp_path[MAX_PATH];
    size_t hosts_len = strlen(hosts_path);
    if (hosts_len + 5 >= sizeof(tmp_path)) {
        fclose(file);
        fprintf(stderr, "[HOSTS] hosts path too long for temp path\n");
        return 1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", hosts_path);

    FILE* tmp_file = open_hosts_file(tmp_path, "wb");
    if (!tmp_file) {
        fclose(file);
        fprintf(stderr, "[HOSTS] Failed to open temporary hosts file: %s\n", tmp_path);
        return 1;
    }

    char line[1024];
    bool any_removed = false;
    while (fgets(line, sizeof(line), file)) {
        char* end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == '\n')) {
            end[-1] = '\0';
            end--;
        }

        if (strcmp(line, MARKER_LINE) == 0 || strcmp(line, HOSTS_ENTRY_1) == 0 || strcmp(line, HOSTS_ENTRY_2) == 0) {
            any_removed = true;
            continue;
        }

        fprintf(tmp_file, "%s\r\n", line);
    }

    fclose(file);
    fclose(tmp_file);

    if (!MoveFileExA(tmp_path, hosts_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "[HOSTS] Failed to replace hosts file: %lu\n", GetLastError());
        remove(tmp_path);
        return 1;
    }

    if (any_removed) {
        printf("[HOSTS] Proxy block removed.\n");
    } else {
        printf("[HOSTS] No proxy block found in hosts file.\n");
    }
    flush_dns_cache();
    return 0;
}

