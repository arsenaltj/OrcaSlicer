#include "../android_publish.hpp"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <initializer_list>
using namespace orca_model_detail;
static int modern_calls, legacy_calls, failure;
static int modern(int from, const char* source, int to, const char* destination, unsigned flags) {
    ++modern_calls;
    assert(from == -100 && to == -100 && flags == 1u);
    assert(std::strcmp(source,"owned.tmp") == 0 && std::strcmp(destination,"result.glb") == 0);
    errno = failure;
    return failure ? -1 : 0;
}
static int legacy(const char*, const char*) { ++legacy_calls; return 0; }
int main() {
    for (int api : {-1,24,29}) {
        modern_calls=legacy_calls=0;
        auto result=android_exclusive_publish(api,modern,legacy,"owned.tmp","result.glb");
        assert(result.status==0 && !result.consumed_temporary && modern_calls==0 && legacy_calls==1);
    }
    modern_calls=legacy_calls=0;
    auto absent=android_exclusive_publish(37,nullptr,legacy,"owned.tmp","result.glb");
    assert(absent.status==0 && !absent.consumed_temporary && modern_calls==0 && legacy_calls==1);
    for (int error : {0,EACCES,ENOSYS,EEXIST,EXDEV}) {
        modern_calls=legacy_calls=0;failure=error;
        auto result=android_exclusive_publish(37,modern,legacy,"owned.tmp","result.glb");
        assert(result.status==(error ? -1 : 0) && result.consumed_temporary==(error==0));
        assert(modern_calls==1 && legacy_calls==0 && errno==error);
    }
}
