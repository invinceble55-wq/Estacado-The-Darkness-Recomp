// Launcher update check (runtime/runtime_update_check.cpp, #24): version
// parsing and choosing the newest regular release from a GitHub list.
#include "runtime_update_check.h"

#include <cstdio>
#include <string>

namespace update = darkness::update_check;

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= Check(update::ParseVersion("0.9.6") == update::Version{0, 9, 6, 0}, "plain version");
    ok &= Check(update::ParseVersion("v0.9.3.1") == update::Version{0, 9, 3, 1}, "four parts");
    ok &= Check(!update::ParseVersion("issue16-test2"), "test build tag");
    ok &= Check(!update::ParseVersion("v0.9.7-rc1"), "suffix");
    ok &= Check(!update::ParseVersion("v1"), "one part");
    ok &= Check(!update::ParseVersion(""), "empty");
    ok &= Check(*update::ParseVersion("0.10.0") > *update::ParseVersion("0.9.6"), "numeric order");

    // Shape of GitHub's answer (newest first, test build and draft mixed in).
    const std::string json = R"([
      {"html_url":"x","id":4,"author":{"login":"a","html_url":"y"},"tag_name":"v0.9.8","draft":true,"prerelease":true},
      {"html_url":"x","id":3,"tag_name":"issue16-test3","draft":false},
      {"html_url":"x","id":2,"tag_name":"v0.9.7","draft":false,"prerelease":true},
      {"html_url":"x","id":1,"tag_name":"v0.9.6","draft":false,"prerelease":true},
      {"html_url":"x","id":0,"tag_name":"v0.9.3.1","draft":false}
    ])";
    const auto newer = update::NewestNewerRelease(json, "0.9.6");
    ok &= Check(newer && newer->version == "0.9.7" && newer->tag == "v0.9.7", "newest regular");
    ok &= Check(newer && newer->url ==
                             std::string(update::kReleasePageUrl) + "v0.9.7",
                "release page");
    ok &= Check(!update::NewestNewerRelease(json, "0.9.7"), "up to date");
    ok &= Check(!update::NewestNewerRelease(json, "1.0"), "newer than every release");
    ok &= Check(!update::NewestNewerRelease(json, ""), "unknown own version");
    const auto fromOld = update::NewestNewerRelease(json, "0.9.3");
    ok &= Check(fromOld && fromOld->version == "0.9.7", "from an old version");
    ok &= Check(!update::NewestNewerRelease("{\"message\":\"rate limited\"}", "0.9.6"), "error answer");

    if (ok) std::printf("runtime_update_check: all checks passed\n");
    return ok ? 0 : 1;
}
