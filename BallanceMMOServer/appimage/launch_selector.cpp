#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

const char* const available_binaries[] = { "Server", "MockClient", "RecordParser" };
const char* target;

bool select_target(char* test_name) {
    for (const char* name: available_binaries) {
        if (std::strcmp(test_name, name) != 0) continue;
        target = name;
        return true;
    }
    return false;
}

int parse_args(int argc, char** argv) {
    using namespace std;
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [OPTION]...\n", argv[0]);
        puts("Options:");
        puts("  -h, --help\t\t Display this help and exit.");
        puts("  -l, --launch [Target]\t Launch the selected target.");
        puts("Additional options will be forwarded to the target.");
        printf("Available targets (default: `%s`):\n", available_binaries[0]);
        for (const char* name: available_binaries)
            printf("  %s\n", name);
        puts("Examples:");
        printf("  To see the server help:\n\t%s --launch Server --help\n", argv[0]);
        printf("  To launch a mock client with a custom name:\n\t%s -l MockClient -n Name\n", argv[0]);
        return 0;
    }
    else if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "--launch") == 0) {
        if (argc <= 2) {
            target = available_binaries[0];
            return argc;
        }
        if (select_target(argv[2])) {
            return 3;
        }
        fprintf(stderr, "Error: target `%s` not found.\n", argv[2]);
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = "$APPDIR/usr/bin/BallanceMMO";

    if (argc < 1) return -1;

    if (argc == 1) {
        target = available_binaries[0];
        argc = 0;
    }
    else {
        int index = parse_args(argc, argv);
        if (index <= 0) return index;
        argc -= index, argv += index;
    }
    cmd += target;
    for (int i = 0; i < argc; ++i) {
        cmd += ' ';
        cmd += argv[i];
    }

    return std::system(cmd.c_str());
}
