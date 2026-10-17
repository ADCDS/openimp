/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void *symbol = dlsym(library, "IMP_System_GetVersion");
    int (*version)(char *);
    memcpy(&version, &symbol, sizeof(version));
    char text[128] = {0};
    if (!version || version(text) != 0 || !strstr(text, "T23 openimp")) {
        fputs("Unexpected OpenIMP version\n", stderr); dlclose(library); return 1;
    }
    printf("Loaded: %s\n", text);
    return dlclose(library) ? 1 : 0;
}
