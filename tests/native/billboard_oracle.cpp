// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <array>
#include "native_oracle.h"
#include "unity/billboard.h"
extern "C" void shittimCameraSizeBlock(const void*,const void*,void*);
extern "C" void shittimCameraLimitPackingBlock(const void*,void*,const void*,const void*);
asm(R"(
.text
.global shittimCameraSizeBlock
.type shittimCameraSizeBlock,@function
shittimCameraSizeBlock:
 push %rbp
 sub $80,%rsp
 mov %rdx,%rbp
 movaps 0(%rsi),%xmm10
 movaps 16(%rsi),%xmm5
 movaps 32(%rsi),%xmm3
 movaps 48(%rsi),%xmm6
 xorps %xmm9,%xmm9
 movaps 64(%rsi),%xmm12
 movaps 80(%rsi),%xmm11
 movaps 96(%rsi),%xmm15
 movaps 112(%rsi),%xmm0
 movaps %xmm0,192(%rbp)
 call *%rdi
 add $80,%rsp
 pop %rbp
 ret
.size shittimCameraSizeBlock,.-shittimCameraSizeBlock
.global shittimCameraLimitPackingBlock
.type shittimCameraLimitPackingBlock,@function
shittimCameraLimitPackingBlock:
 push %rbx
 push %rbp
 push %r14
 push %r15
 sub $296,%rsp
 mov %rsi,%rbx
 mov %rdx,%rbp
 mov %rcx,%r15
 xor %r14d,%r14d
 xorps %xmm0,%xmm0
 call *%rdi
 add $296,%rsp
 pop %r15
 pop %r14
 pop %rbp
 pop %rbx
 ret
.size shittimCameraLimitPackingBlock,.-shittimCameraLimitPackingBlock
)");

int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QFile proof(BA_BILLBOARD_PROOF),dll(SHITTIM_UNITY_PLAYER);
    if(!proof.open(QIODevice::ReadOnly)||!dll.open(QIODevice::ReadOnly))return 1;
    const auto data=QJsonDocument::fromJson(proof.readAll()).object();const auto bytes=dll.readAll();
    if(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()!=data["sha256"].toString().toLatin1())return 1;
    Native pack,clamp;
    if(!pack.load(data["functions"].toObject(),bytes,"billboardLimitPacking")
        || !clamp.load(data["functions"].toObject(),bytes,"quadZClamp"))return 1;
    int cases=0;
    for(float minimum:{0.f,.01f})for(float maximum:{.5f,2.f})for(float scale:{.01539412f,.3078823984f,1.f,1.539412f,5.f})
    for(float span:{.3333333f,1.125f,2.f,3.5555556f,7.111111f})for(float size:{0.f,.05f,.1f,.2f,1.f,4.f}) {
        alignas(16) std::array<unsigned char,512> context{},draw{};
        const std::array<float,3> renderer{0,minimum,maximum};
        const float far=20.f;put(context.data(),0x120,scale);put(draw.data(),0xe0,span);put(draw.data(),0xe8,far);draw[0xec]=1;
        shittimCameraLimitPackingBlock(reinterpret_cast<void*>(pack.update),context.data(),draw.data(),renderer.data());
        const auto limits=baclickfx::unity::particleBillboardLimits(minimum,maximum,scale,span,far,true);
        float lo,hi;std::memcpy(&lo,context.data()+0x134,4);std::memcpy(&hi,context.data()+0x138,4);
        if(lo!=limits[2]||hi!=limits[3])return 1;
        alignas(16) std::array<std::array<float,4>,8> input{};
        input[0].fill(size);input[1].fill(size);input[2].fill(lo);input[3].fill(hi);
        input[4].fill(1e-6f);input[5].fill(.5f);input[6].fill(100.f);
        alignas(16) std::array<unsigned char,8192> scratch{};
        shittimCameraSizeBlock(reinterpret_cast<void*>(clamp.update),input.data(),scratch.data());
        const auto expected=baclickfx::unity::particleBillboardHalfExtents(size,size,lo,hi);
        float x,y;std::memcpy(&x,scratch.data()+0x470,4);std::memcpy(&y,scratch.data()+0x480,4);
        if(x!=expected[0]||y!=expected[1])return 1;++cases;
    }
    std::printf("Original billboard packing/clamp: %d source/viewport/scale/size probes, exact half-extents.\n",cases);
}
