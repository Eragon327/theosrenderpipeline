#pragma once
namespace TheosRenderPipeline::NeuralRendering
{
// Objects and Depth are declared by each caller. Use the same classification,
// depth rejection and signed jitter convention for preview and vendor controls.
// Peripheral mapping matches SourceDLSSGNeuralResolveShader.h; its optimizer-
// fps-dlss5 attribution and MIT notice are retained in THIRD-PARTY.md.
inline constexpr char kObjectGuideFunctions[] = R"(
float ObjectUnpackRadius(float r) {
    if(r<=0.8) return r;
    if(r>0.9) return 1+(r-0.9)/0.25;
    float y=(r-0.8)/0.1; return 0.8+0.2*(0.5*y/(1-0.5*y));
}
uint ClassifiedObject(float2 pixel,uint2 size,uint2 guides,float2 offset,uint peripheral) {
    float2 uv=pixel/float2(size);
    if(peripheral) {
        float2 r=(2*uv-1)*0.9;
        uv=(sign(r)*float2(ObjectUnpackRadius(abs(r.x)),ObjectUnpackRadius(abs(r.y)))+1)*0.5;
    }
    int2 coordinate=int2(floor(uv*float2(guides)+offset));
    uint2 q=uint2(clamp(coordinate,int2(0,0),int2(guides)-1));
    uint packed=asuint(Objects.Load(int3(q,0))); float depth=Depth.Load(int3(q,0));
    uint group=packed&7u; float objectDepth=asfloat(packed&~7u);
    return isfinite(objectDepth)&&isfinite(depth)&&group>=1&&group<=4&&
        abs(objectDepth-depth)<=max(0.00000024,abs(depth)*0.000002) ? group : 0;
}
)";
}
