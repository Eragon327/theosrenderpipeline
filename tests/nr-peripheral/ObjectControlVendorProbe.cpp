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
    std::vector<Pixel> baseline;double uniformMax=0,changedMean=0;
    for(unsigned mode=0;mode<3;++mode){
        NeuralOptions options;options.enabled=true;options.beforeUpscaling=true;options.runtimePath=std::filesystem::absolute(argv[1]);
        options.tuning.intensity=.8f;options.tuning.localToneStrength=.4f;options.tuning.localStructureStrength=.6f;
        options.objectMasks.enabled=true;options.objectMasks.strength.fill(1);
        if(mode)for(unsigned group=0;group<4;++group)
            options.objectMasks.passes[0].groups[group]={true,mode==2?float(group%2):.4f,mode==2?float((group+1)%2):.6f};
        NeuralPass pass;
        for(unsigned frame=0;frame<8;++frame){
            gpu.Begin();Check(Interop::RecordCopy(gpu.list.Get(),original.Get(),scene.Get()),"fresh scene");
            Require(pass.Record(gpu.device.Get(),gpu.list.Get(),frame%kCommandSlots,options,frame==0,true,w,h,
                motion.Get(),depth.Get(),nullptr,scene.Get(),nullptr,0,{},mode?mask.Get():nullptr),pass.Status().c_str());gpu.End();
        }
        const auto result=gpu.Read(pass.Corrected());
        for(const auto& p:result)for(float c:p)Require(std::isfinite(c),"real NR output finite");
        if(!mode){baseline=result;continue;}
        for(unsigned i=0;i<w*h;++i)for(unsigned c=0;c<3;++c){
            const double error=std::abs(double(result[i][c])-baseline[i][c]);
            if(mode==1)uniformMax=std::max(uniformMax,error);else changedMean+=error/(w*h*3);
        }
        std::printf("mode=%u status=%s\n",mode,pass.Status().c_str());
    }
    std::printf("uniform_mask_max_abs=%g changed_groups_mean_abs=%g\n",uniformMax,changedMean);
    Require(uniformMax<=.00001,"uniform control mask reproduces equivalent global tuning");
    Require(changedMean>.00001,"group tone/structure controls change actual provider pixels");
    std::puts("PASS actual NR control-mask channel contract; no visual or cadence acceptance implied");
}
