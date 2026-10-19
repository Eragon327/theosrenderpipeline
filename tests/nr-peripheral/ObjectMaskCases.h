#pragma once
// Execute masks inside the actual NR recorder with a nonidentity feature double.
// This qualifies preparation/resolve/UI/state handling, not NVIDIA model quality.
static void ObjectMaskCases(GPU& gpu)
{
    using namespace TheosRenderPipeline::NeuralRendering;
    constexpr unsigned w=64,h=32,gw=32,gh=16;
    for(bool world:{false,true})for(int passes:{1,2})for(bool peripheral:{false,true})for(bool jittered:{false,true}) {
        const std::array<float,2> offset=jittered?std::array<float,2>{.75f,-.375f}:std::array<float,2>{};
        NeuralOptions options; options.enabled=true; options.beforeUpscaling=world; options.passes=passes;
        options.runtimePath="fixture.dll"; options.tuning.intensity=.8f; options.tuning.uiCorrection=!world;
        options.reconstruction.method=ResolveMethod::Ratio; options.reconstruction.inputScale=.75f;
        options.reconstruction.peripheralCompression=peripheral;
        options.secondPass.linked=false; options.secondPass.inputScale=.5f; options.secondPass.preset=1;
        options.secondPass.tuning.intensity=.6f; options.secondPass.tuning.uiCorrection=!world;
        options.objectMasks={true,{0,.25f,.5f,1}};
        Fixture::peripheral=peripheral; Fixture::worldOnly=world; Fixture::sourceWidth=w; Fixture::sourceHeight=h;
        Fixture::guideWidth=gw; Fixture::guideHeight=gh; Fixture::transform=true;
        Fixture::workWidth=ModelExtent(w,options.reconstruction); Fixture::workHeight=ModelExtent(h,options.reconstruction);
        auto second=options.reconstruction; second.inputScale=.5f;
        Fixture::secondWidth=ModelExtent(w,second); Fixture::secondHeight=ModelExtent(h,second);
        auto motion=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{}),DXGI_FORMAT_R32G32_FLOAT);
        auto depth=gpu.Texture(gw,gh,std::vector<Pixel>(gw*gh,{.4f,0,0,0}),DXGI_FORMAT_R32_FLOAT);
        auto ui=gpu.Texture(w,h,std::vector<Pixel>(w*h,{.05f,.025f,0,.25f})),composed=gpu.Texture(w,h);
        const std::vector<Pixel> pixels(w*h,{.2f,.3f,.4f,.5f});
        std::vector<Pixel> classes(gw*gh);
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x)
            classes[y*gw+x]={PackObjectPixel(static_cast<ObjectGroup>(x%4+1),y%3==0?.2f:.4f),0,0,0};
        auto mask=gpu.Texture(gw,gh,classes,DXGI_FORMAT_R32_FLOAT);
        std::vector<Pixel> baseline;
        for(bool masked:{false,true}) {
            Fixture::creations=Fixture::evaluations=Fixture::resets=0;
            NeuralPass pass;
            auto scene=gpu.Texture(w,h,pixels);
            gpu.Begin(); Require(pass.Record(gpu.device.Get(),gpu.list.Get(),0,options,true,true,float(gw),float(gh),
                motion.Get(),depth.Get(),world?nullptr:ui.Get(),scene.Get(),world?nullptr:composed.Get(),0,{},masked?mask.Get():nullptr,offset),pass.Status().c_str()); gpu.End();
            const auto actual=gpu.Read(pass.Corrected());
            if(!masked){baseline=actual;continue;}
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
                const auto i=y*w+x; const auto qx=unsigned(std::clamp(int(std::floor((x+.5f)*gw/w+offset[0])),0,int(gw)-1)),
                    qy=unsigned(std::clamp(int(std::floor((y+.5f)*gh/h+offset[1])),0,int(gh)-1));
                const float amount=qy%3==0?1:options.objectMasks.strength[qx%4];
                for(unsigned c=0;c<4;++c) {
                    const float expected=amount==0?pixels[i][c]:amount==1?baseline[i][c]:c==3?pixels[i][c]:
                        pixels[i][c]+amount*(baseline[i][c]-pixels[i][c]);
                    Near(actual[i][c],expected,.000001,"mask applies to final one/two-pass native-grid result");
                }
            }
            if(!world) {
                const auto real=gpu.Read(pass.Composed()),tag=gpu.Read(scene.Get());
                for(unsigned i=0;i<w*h;++i)for(unsigned c=0;c<4;++c) {
                    Near(tag[i][c],actual[i][c],0,"masked real frame and FG HUD-less input agree");
                    if(c<3)Near(real[i][c],actual[i][c]*.75f+std::array<float,3>{.05f,.025f,0}[c],.000001,"UI composes after object masks");
                }
            }
        }
    }
    Fixture::secondWidth=Fixture::secondHeight=0;Fixture::transform=false;
    std::puts("PASS object masks inside production NR recorder: both placements, one/two independent passes, scaled/peripheral grids, UI and FG tags");
}
