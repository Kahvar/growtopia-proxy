#include "hosts.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>


#define HOSTS_PATH "C:\\Windows\\System32\\drivers\\etc\\hosts"
#define MARKER "# Proxy\r\n"


int add_hosts(void) {
    FILE* file = fopen(HOSTS_PATH, "a");
    if (file == NULL) {
        perror("Couldn't write to hosts file for some reason.\nTry running the proxy with admin priviledges if that helps.");
        return 1;
    }

    fprintf(file, MARKER);
    fprintf(file, "127.0.0.1 growtopia1.com\r\n");
    fprintf(file, "127.0.0.1 www.growtopia1.com\r\n");

    fclose(file);
    return 0;
}

int remove_hosts(void) {
    FILE* src = fopen(HOSTS_PATH, "r");
    if (src == NULL) {
        perror("fopen (read) failed");
        return 1;
    }

    FILE* tmp = fopen("hosts.tmp", "w");
    if (tmp == NULL) {
        perror("fopen (tmp) failed");
        fclose(src);
        return 1;
    }

    char line[512];
    int lines_to_skip = 0;

    while (fgets(line, sizeof(line), src)) {
        if (strstr(line, MARKER) != NULL) {
            lines_to_skip = 3; // 1 marker-rivi + 2 host-riviä (paivita jos add_hosts:in rivimäärä muuttuu)
        }

        if (lines_to_skip > 0) {
            lines_to_skip--;
            continue;
        }

        fputs(line, tmp);
    }

    fclose(src);
    fclose(tmp);

    if (remove(HOSTS_PATH) != 0) {
        perror("remove failed");
        return 1;
    }
    if (rename("hosts.tmp", HOSTS_PATH) != 0) {
        perror("rename failed");
        return 1;
    }

    printf("[HOSTS] Redirects cleared.\n");
    return 0;
}