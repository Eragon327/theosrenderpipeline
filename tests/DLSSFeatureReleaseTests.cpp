#include "DLSSFeatureRelease.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static void Require(bool value,const char* reason)
{
    if(!value) {std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}
}
int main()
{
    NVSDK_NGX_Handle object{};
    auto* owned=&object;
    bool resources=true;
    std::vector<int> events;
    const auto failed=TheosRenderPipeline::DLSS::ReleaseOwnedFeature(owned,
        [&](NVSDK_NGX_Handle* handle) {Require(handle==&object,"release receives owned feature");events.push_back(1);return NVSDK_NGX_Result_FAIL_PlatformError;},
        [&] {events.push_back(2);resources=false;});
    Require(failed==NVSDK_NGX_Result_FAIL_PlatformError,"original SDK failure is returned");
    Require(owned==&object && resources && events==std::vector<int>{1},"failed release retains feature and all dependent resources");
    const auto retried=TheosRenderPipeline::DLSS::ReleaseOwnedFeature(owned,
        [&](NVSDK_NGX_Handle* handle) {Require(handle==&object,"retry receives original feature");events.push_back(1);return NVSDK_NGX_Result_Success;},
        [&] {Require(owned==nullptr,"feature is relinquished before dependent cleanup");events.push_back(2);resources=false;});
    Require(NVSDK_NGX_SUCCEED(retried) && !owned && !resources && events==std::vector<int>{1,1,2},"successful retry releases dependencies once");
    unsigned sdkCalls=0,cleanups=0;
    TheosRenderPipeline::DLSS::ReleaseOwnedFeature(owned,
        [&](NVSDK_NGX_Handle*) {++sdkCalls;return NVSDK_NGX_Result_Success;},[&] {++cleanups;});
    Require(!sdkCalls && cleanups==1,"partial allocation without feature cleans up without an SDK call");
    std::puts("PASS SDK release failure retention, retry ordering and partial allocation cleanup");
}
