// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include "native_oracle.h"
#include "unity/lineribbon.h"
#include "unity/gradient.h"
struct Plan {std::uint64_t target,parent,size,count;};
std::vector<Plan> plans;std::vector<void*> allocations;
void* alloc(std::size_t n){void *p=nullptr;posix_memalign(&p,16,(n+15)&~size_t(15));std::memset(p,0,n);allocations.push_back(p);return p;}
extern "C" void __attribute__((ms_abi)) init(void *){plans.clear();}
extern "C" void __attribute__((ms_abi)) plan(void*,std::uint64_t target,std::uint64_t parent,std::uint64_t size,std::uint64_t count,std::uint64_t,std::uint64_t,std::uint64_t){plans.push_back({target,parent,size,count});}
extern "C" void __attribute__((ms_abi)) allocate(void*,unsigned,unsigned){
 auto *root=(unsigned char*)alloc(plans[0].size*plans[0].count);std::memcpy((void*)plans[0].target,&root,8);
 for(size_t i=1;i<plans.size();++i){auto *p=alloc(plans[i].size*plans[i].count);std::memcpy(root+plans[i].target,&p,8);}
}
extern "C" void __attribute__((ms_abi)) release(void*,unsigned,void*,unsigned){for(auto p:allocations)free(p);allocations.clear();}

int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    QFile proof(BA_TRAIL_MESH_PROOF),dllFile(SHITTIM_UNITY_PLAYER),source(BA_TOUCH_SOURCE);
    if(!proof.open(QIODevice::ReadOnly)||!dllFile.open(QIODevice::ReadOnly)||!source.open(QIODevice::ReadOnly))return 1;
    const auto data=QJsonDocument::fromJson(proof.readAll()).object();const auto dll=dllFile.readAll();
    if(QCryptographicHash::hash(dll,QCryptographicHash::Sha256).toHex()!=data["sha256"].toString().toLatin1())return 1;
    Native native;
    if(!native.load(data["functions"].toObject(),dll,"line",{{"0x284210",(uintptr_t)init},{"0x2850d0",(uintptr_t)plan},
        {"0x284600",(uintptr_t)allocate},{"0x2806d0",(uintptr_t)release}}))return 1;
    const auto authored=QJsonDocument::fromJson(source.readAll()).object()["trail"].toObject()["m_Parameters"].toObject();
    alignas(16) unsigned char params[512]{};
    struct Key{float t,v,in,out;unsigned weighted;float iw,ow;};Key key{0,1,0,0,0,0,0};
    put(params,8,float(authored["widthMultiplier"].toDouble()));
    put(params,0x10,-1);put(params,0x14,-__builtin_inff());put(params,0x18,-__builtin_inff());
    put(params,0x48,&key);put(params,0x58,uint64_t(1));put(params,0x68,2);put(params,0x6c,2);
    const auto gradient=authored["colorGradient"].toObject();
    auto *g=params+0x78;baclickfx::unity::Gradient portGradient;portGradient.enabled=true;
    for(int i=0;i<8;++i){auto c=gradient[QString("key%1").arg(i)].toObject();
        for(int ch=0;ch<4;++ch)put(g,i*16+ch*4,float(c[QString("rgba").mid(ch,1)].toDouble()));
        const auto ct=uint16_t(gradient[QString("ctime%1").arg(i)].toInt()),at=uint16_t(gradient[QString("atime%1").arg(i)].toInt());
        put(g,0x80+i*2,ct);put(g,0x90+i*2,at);
        if(i<gradient["m_NumColorKeys"].toInt())portGradient.colors.push_back({ct,{float(c["r"].toDouble()),float(c["g"].toDouble()),float(c["b"].toDouble())}});
        if(i<gradient["m_NumAlphaKeys"].toInt())portGradient.alphas.push_back({at,float(c["a"].toDouble())});
    }
    put(g,0xa0,uint8_t(gradient["m_NumColorKeys"].toInt()));put(g,0xa1,uint8_t(gradient["m_NumAlphaKeys"].toInt()));
    put(params,0x120,4);put(params,0x124,1);put(params,0x128,authored["alignment"].toInt());put(params,0x12c,0);
    const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    using Line=void(__attribute__((ms_abi)) *)(void*,void*,const void*,const void*,const void*,const void*,uint64_t,bool,float);
    std::vector<std::vector<std::array<float,3>>> paths{
        {{0,0,0},{.1f,0,0}},{{0,0,0},{.1f,0,0},{.1f,.1f,0}},
        {{0,0,0},{.1f,0,0},{.1f,-.1f,0}},{{0,0,0},{.1f,.1f,0},{.2f,0,0}},
        {{0,0,0},{.1f,.1f,0},{.1001f,.0999f,0}},{{0,0,0},{0,0,0},{0,0,0}},
        {{0,0,0},{.1f,0,0},{0,0,0}},{{.5f,-.2f,0},{.3f,.3f,0},{.30001f,.3f,0},{.5f,-.2f,0}}
    };
    for(unsigned seed=0;seed<64;++seed){std::vector<std::array<float,3>> points;float x=0,y=0;
        unsigned state=seed+1;for(int i=0;i<8;++i){state=state*1664525+1013904223;x+=float(int(state&1023)-512)*.0001f;
            state=state*1664525+1013904223;y+=float(int(state&1023)-512)*.0001f;points.push_back({x,y,0});}paths.push_back(points);}
    QJsonArray snapshot;std::size_t cases=0,checked=0;float maximumError=0;
    for(const auto &points:paths) {
        alignas(16) std::array<unsigned char,65536> vertices;vertices.fill(0xcd);
        std::vector<float> flags(points.size(),1.f);
        reinterpret_cast<Line>(native.update)(vertices.data(),params,identity.data(),identity.data(),points.data(),flags.data(),points.size(),false,0);
        std::size_t count=0;while(count*24+24<=vertices.size()
            && !std::all_of(vertices.begin()+count*24,vertices.begin()+(count+1)*24,[](unsigned char b){return b==0xcd;}))++count;
        std::vector<baclickfx::unity::LinePoint> input;for(const auto &p:points)input.push_back({p[0],p[1]});
        baclickfx::unity::LineRibbonScratch scratch;std::vector<baclickfx::unity::LineRibbonVertex> port;
        baclickfx::unity::buildLineRibbon(input,float(authored["widthMultiplier"].toDouble()),4,1,scratch,port);
        if(count!=port.size()){std::fprintf(stderr,"Trail mesh count case=%zu %zu/%zu\n",cases,count,port.size());return 1;}
        QJsonArray expected,sourcePoints;
        for(const auto &p:points)sourcePoints.append(QJsonArray{double(p[0]),double(p[1])});
        for(std::size_t i=0;i<count;++i){float fields[6];std::memcpy(fields,vertices.data()+i*24,24);
            const auto &v=port[i];const float error=std::max(std::abs(fields[0]-v.position[0]),std::abs(fields[1]-v.position[1]));
            maximumError=std::max(maximumError,error);
            if(error != 0 || fields[4]!=v.u || fields[5]!=v.v) {
                std::fprintf(stderr,"Trail mesh case=%zu vertex=%zu error=%a pos=%a,%a/%a,%a uv=%a,%a/%a,%a\n",cases,i,error,
                    fields[0],fields[1],v.position[0],v.position[1],fields[4],fields[5],v.u,v.v);return 1;}
            const auto color=portGradient.evaluate(v.u);std::array<uint8_t,4> packed;
            for(int ch=0;ch<4;++ch)packed[size_t(ch)]=baclickfx::unity::colorByte(color[size_t(ch)]);
            if(std::memcmp(vertices.data()+i*24+12,packed.data(),4)){std::fprintf(stderr,"Trail color case=%zu vertex=%zu\n",cases,i);return 1;}
            std::uint32_t rgba=0;std::memcpy(&rgba,vertices.data()+i*24+12,4);
            expected.append(QJsonArray{double(fields[0]),double(fields[1]),double(fields[4]),double(fields[5]),double(rgba)});++checked;
        }
        if(cases<8)snapshot.append(QJsonObject{{"points",sourcePoints},{"vertices",expected}});
        ++cases;
    }
    QFile destination(BA_NATIVE_SNAPSHOT);if(!destination.open(QIODevice::WriteOnly))return 1;
    destination.write(QJsonDocument(QJsonObject{{"unityPlayerSha256",data["sha256"]},{"samples",snapshot}}).toJson(QJsonDocument::Compact));
    std::printf("Original Trail mesh: %zu paths, %zu vertices, exact vertex counts/UV-v/colors; max planar coordinate error %.9g world units; source corners/caps, zero/reversed/short/random segments.\n",cases,checked,maximumError);
}
