#include "FrameGen/SourceRetirementPolicy.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace TheosRenderPipeline::SourceDLSSG;
static void Require(bool b,const char* s){if(!b){std::fprintf(stderr,"FAIL %s\n",s);std::exit(1);}}
int main(){
    for(auto purpose:{RetirementPurpose::Resize,RetirementPurpose::UpscalerReplacement})
    for(bool completion:{false,true}){
        bool readerActive=true;unsigned guideIdentity=17,featureIdentity=23;bool historyValid=true;
        std::vector<int> events;
        const bool result=RetireSharedResources(purpose,[&]{events.push_back(1);if(completion)readerActive=false;return completion;},
            [&]{Require(!readerActive,"harvest follows completion");events.push_back(2);},
            [&]{Require(!readerActive,"release never races a reader");events.push_back(3);guideIdentity=featureIdentity=0;},
            [&]{Require(!readerActive,"history reset follows retirement");events.push_back(4);historyValid=false;});
        Require(result==completion,"retirement result propagates");
        if(!completion){Require(events==std::vector<int>{1}&&guideIdentity==17&&featureIdentity==23&&historyValid,"failure retains owners and history");}
        else if(purpose==RetirementPurpose::Resize){Require(events==std::vector<int>{1,2,3,4}&&!guideIdentity&&!featureIdentity&&!historyValid,"resize harvests releases resets");}
        else {Require(events==std::vector<int>{1,2,4}&&guideIdentity==17&&featureIdentity==23&&!historyValid,"fixed-size replacement retains identities but invalidates history");}
    }
    std::puts("PASS resize versus fixed-size retention, ordering and failure ownership");
}
