#include "swarm_test.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <string>
#include <vector>

int main()
{
    const char *temporary = std::getenv("TMPDIR");
    auto base = std::filesystem::path(temporary ? temporary : ".") / "opence-swarm-XXXXXX";
    std::string pattern = base.string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    if (!mkdtemp(name.data())) return 2;
    char report[1024] = {};
    int result = opence_probe_tcp(name.data(), report, sizeof(report));
    std::puts(report);
    std::filesystem::remove_all(name.data());
    return result;
}
