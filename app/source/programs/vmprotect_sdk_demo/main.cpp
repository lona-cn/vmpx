#if !defined(_WIN32) || !defined(_M_X64)
#error "vmprotect_sdk_demo requires Windows x64"
#endif

#include <VMProtectSDK.h>

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[])
{
    if (argc != 2 || (std::strcmp(argv[1], "--expect-protected") != 0 &&
                      std::strcmp(argv[1], "--expect-unprotected") != 0))
    {
        std::fprintf(stderr, "Usage: vmprotect_sdk_demo --expect-protected|--expect-unprotected\n");
        return 2;
    }

    const bool expected = std::strcmp(argv[1], "--expect-protected") == 0;
    const bool protected_image = VMProtectIsProtected();
    std::printf("VMProtectIsProtected=%s\n", protected_image ? "true" : "false");
    return protected_image == expected ? 0 : 1;
}
