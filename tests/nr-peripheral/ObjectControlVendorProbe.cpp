// Manual opt-in check of the real provider's control-mask channel contract.
// No game, display presentation or timing claim. Run with a qualified NR DLL.
#include "GPU.h"
#include <filesystem>

int wmain(int argc,wchar_t** argv)
{
    Require(argc==2,"usage: TRPNeuralObjectVendorProbe absolute-qualified-NR-DLL");
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    constexpr unsigned w=640,h=384;
    GPU gpu(true);
    std::vector<Pixel> pixels(w*h),classes(w*h);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        pixels[y*w+x]={.1f+.6f*x/w,.1f+.6f*y/h,.2f+.3f*((x/8+y/8)%2),1};
        classes[y*w+x]={TheosRenderPipeline::NeuralRendering::PackObjectPixel(
            static_cast<TheosRenderPipeline::NeuralRendering::ObjectGroup>(1+x/(w/4)),.5f),0,0,0};
    }
    auto original=gpu.Texture(w,h,pixels,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto scene=gpu.Texture(w,h,{},DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=gpu.Texture(w,h,std::vector<Pixel>(w*h,{}),DXGI_FORMAT_R32G32_FLOAT);
    auto depth=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.5f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
    auto mask=gpu.Texture(w,h,classes,DXGI_FORMAT_R32_FLOAT);
    for(const auto tuning:{std::array<float,3>{.8f,.4f,.6f},std::array<float,3>{.6f,0,1},std::array<float,3>{.9f,1,0}}){
    std::vector<Pixel> baseline;double uniformMax=0,changedMean=0,repeatMax=0;
    NeuralPass pass;
    for(unsigned mode=0;mode<4;++mode){
        const bool masked=mode==1||mode==2;
        NeuralOptions options;options.enabled=true;options.beforeUpscaling=true;options.runtimePath=std::filesystem::absolute(argv[1]);
        options.tuning.intensity=tuning[0];options.tuning.localToneStrength=tuning[1];options.tuning.localStructureStrength=tuning[2];
        if(masked)for(unsigned group=0;group<4;++group)
            options.objectMasks.passes[0].groups[group]={true,mode==2?float(group%2):tuning[1],mode==2?float((group+1)%2):tuning[2]};
        for(unsigned frame=0;frame<8;++frame){
            gpu.Begin();Check(Interop::RecordCopy(gpu.list.Get(),original.Get(),scene.Get()),"fresh scene");
            Require(pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0,true,w,h,
                motion.Get(),depth.Get(),nullptr,scene.Get(),nullptr,0,{},masked?mask.Get():nullptr),pass.Status().c_str());gpu.End();
        }
        const auto result=gpu.Read(pass.Corrected());
        for(const auto& p:result)for(float c:p)Require(std::isfinite(c),"real NR output finite");
        if(!mode){baseline=result;continue;}
        for(unsigned i=0;i<w*h;++i)for(unsigned c=0;c<3;++c){
            const double error=std::abs(double(result[i][c])-baseline[i][c]);
            if(mode==1)uniformMax=std::max(uniformMax,error);else if(mode==2)changedMean+=error/(w*h*3);else repeatMax=std::max(repeatMax,error);
        }
        std::fprintf(stderr,"mode=%u status=%s\n",mode,pass.Status().c_str());
    }
    std::fprintf(stderr,"intensity=%g tone=%g structure=%g uniform_mask_max_abs=%g changed_groups_mean_abs=%g baseline_repeat_max_abs=%g\n",tuning[0],tuning[1],tuning[2],uniformMax,changedMean,repeatMax);
    // The real provider is not bit deterministic after history resets. Bound
    // both comparisons by a small half-float precision envelope below 1, rather than silently
    // attributing repeat noise to the mask. The fixture's RGB stays below 1.
    constexpr double halfUlp=.00048828125;
    Require(repeatMax<=4*halfUlp,"reset no-mask variation stays within four half-float ULPs");
    Require(uniformMax<=4*halfUlp && uniformMax<=repeatMax+halfUlp,
        "uniform control matches global tuning within measured provider repeat precision");
    Require(changedMean>.00001,"group tone/structure controls change actual provider pixels");
    }
    std::fprintf(stderr,"PASS actual NR control-mask channel contract; no visual or cadence acceptance implied\n");
}
