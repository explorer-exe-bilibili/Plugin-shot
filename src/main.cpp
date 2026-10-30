#include <cstdio>
#include "version.h"

int main() {
    std::printf("%s %s\n", PROJECT_NAME_STR, VERSION_FULL);
    std::printf("  %s\n", PROJECT_DESCRIPTION_STR);
    std::printf("  %s | %s %s | %s\n",
                PLATFORM_NAME_STR, COMPILER_NAME_STR,
                COMPILER_VERSION, ARCHITECTURE_STR);
    std::printf("  build %s (%s@%s)\n", BUILD_TYPE_STR, GIT_BRANCH, GIT_COMMIT_HASH);
    std::printf("  built at %s %s\n", BUILD_DATE, BUILD_TIME);
#ifdef PLATFORM_WSL
    std::printf("  [WSL build]\n");
#endif
    return 0;
}