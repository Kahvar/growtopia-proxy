#include "hosts.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#define MARKER_LINE "# Proxy"
#define HOSTS_ENTRY_1 "127.0.0.1 growtopia1.com"
#define HOSTS_ENTRY_2 "127.0.0.1 www.growtopia1.com"

static const char HOSTS_BLOCK[] =
    "# Proxy\r\n"
    "127.0.0.1 growtopia1.com\r\n"
    "127.0.0.1 www.growtopia1.com\r\n";

/*
 * hosts.c — cmd-based hosts management + dnsapi flush support
 * Replaces the previous direct fwrite approach with cmd.exe operations
 * and adds a dynamic DnsFlushResolverCache() call (LoadLibrary/GetProcAddress).
 * Keep this file minimal and safe: commands are quoted and use Sysnative when
 * running under Wow64 so the real System32 hosts is targeted.
 */

#include "hosts.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

#define MARKER_LINE "# Proxy"
#define HOSTS_ENTRY_1 "127.0.0.1 growtopia1.com"
#define HOSTS_ENTRY_2 "127.0.0.1 www.growtopia1.com"

static const char HOSTS_BLOCK[] =
    "# Proxy\r\n"
    "127.0.0.1 growtopia1.com\r\n"
    "127.0.0.1 www.growtopia1.com\r\n";

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

static bool hosts_block_present(const char* path) {
    FILE* file = open_hosts_file(path, "rb");
    if (file == NULL) return false;

    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size < 0) { fclose(file); return false; }
    fseek(file, 0, SEEK_SET);

    char* buffer = malloc((size_t)size + 1);
    if (buffer == NULL) { fclose(file); return false; }

    size_t read = fread(buffer, 1, (size_t)size, file);
    buffer[read] = '\0';
    fclose(file);

    bool found = strstr(buffer, MARKER_LINE) != NULL;
    free(buffer);
    return found;
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

/* Append hosts entries by invoking cmd.exe (del + echo) to mimic the known-working manual steps.
   This helps avoid ACL/ownership differences that sometimes happened when creating temp files elsewhere. */
int add_hosts(void) {
    char hosts_path[MAX_PATH];
    get_hosts_path(hosts_path, sizeof(hosts_path));
    printf("[HOSTS] Using hosts file: %s\n", hosts_path);

    if (hosts_block_present(hosts_path)) {
        printf("[HOSTS] Redirect block already present, no change needed.\n");
        return 0;
    }

    char cmd[2048];
    int n = _snprintf(cmd, sizeof(cmd),
        "cmd /C \"if exist \"%s\" (del /F /Q \"%s\" ) 1>nul 2>&1 & "
        "echo 127.0.0.1 growtopia1.com > \"%s\" & "
        "echo 127.0.0.1 www.growtopia1.com >> \"%s\"\"",
        hosts_path, hosts_path, hosts_path, hosts_path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        fprintf(stderr, "[HOSTS] command too long\n");
        return 1;
    }

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[HOSTS] command failed (rc=%d)\n", rc);
        return 1;
    }

    printf("[HOSTS] Redirect block written using cmd.exe.\n");
    flush_dns_cache();
    return 0;
}

/* Remove the proxy block by writing a filtered tmp file in the same directory and moving it atomically. */
int remove_hosts(void) {
    char hosts_path[MAX_PATH];
    get_hosts_path(hosts_path, sizeof(hosts_path));
    printf("[HOSTS] Removing proxy block from hosts file: %s\n", hosts_path);

    char tmp_path[MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", hosts_path);

    char cmd[4096];
    int n = _snprintf(cmd, sizeof(cmd),
        "cmd /C \"(if exist \"%s\" (findstr /V /C:\"%s\" /C:\"%s\" /C:\"%s\" \"%s\" > \"%s\") else (type nul > \"%s\")) & move /Y \"%s\" \"%s\" >nul 2>&1\"",
        hosts_path, MARKER_LINE, HOSTS_ENTRY_1, HOSTS_ENTRY_2, hosts_path, tmp_path, tmp_path, tmp_path, hosts_path);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        fprintf(stderr, "[HOSTS] remove command too long\n");
        return 1;
    }

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "[HOSTS] remove command failed (rc=%d)\n", rc);
        return 1;
    }

    printf("[HOSTS] Proxy block removed (via cmd pipeline).\n");
    flush_dns_cache();
    return 0;
}

