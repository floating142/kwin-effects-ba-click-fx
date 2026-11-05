// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include "native_oracle.h"
#include "unity/emission.h"

int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    QFile proof(BA_DISTANCE_PROOF), dllFile(SHITTIM_UNITY_PLAYER);
    if (!proof.open(QIODevice::ReadOnly)||!dllFile.open(QIODevice::ReadOnly))return 1;
    const auto data=QJsonDocument::fromJson(proof.readAll()).object();
    const auto dll=dllFile.readAll();
    if(QCryptographicHash::hash(dll,QCryptographicHash::Sha256).toHex()!=data["sha256"].toString().toLatin1())return 1;
    Native native;if(!native.load(data["functions"].toObject(),dll,"distance"))return 1;
    using Distance=std::uint64_t(__attribute__((ms_abi)) *)(void *,void *,const void *,float,float,float,float);
    auto distance=reinterpret_cast<Distance>(native.update);
    int cases=0;
    for(unsigned seed:{0u,1u,42u,0xffffffffu})for(float rate:{0.f,5.f,17.3f})
    for(float carry:{0.f,.25f,.99999994f})for(float dt:{.001f,1.f/240,1.f/144,1.f/60,.1f,.5f})
    for(float speed:{0.f,.1f,1.f,12.37f,128.f}) {
        alignas(16) unsigned char state[64]{}, module[256]{};
        baclickfx::unity::EmissionState port;port.init(seed);port.carry=carry;
        put(state,4,carry);put(state,8,port.random.state());put(module,0x34,rate);
        const std::array<float,3> velocity{speed,-speed*.75f,speed*.125f};
        for(int tick=0;tick<16;++tick) {
            const auto actual=distance(state,module,velocity.data(),0,.3f,dt,1.f);
            const auto expected=baclickfx::unity::advanceDistanceEmission(port,rate,velocity,dt);
            float scale,rest;std::memcpy(&scale,state,4);std::memcpy(&rest,state+4,4);
            if(actual!=expected||std::memcmp(&rest,&port.carry,4)||std::memcmp(&scale,&port.intervalScale,4)
                ||std::memcmp(state+8,port.random.state().data(),16)) {
                std::fprintf(stderr,"Distance mismatch seed=%u rate=%g dt=%g tick=%d count=%llu/%llu carry=%a/%a scale=%a/%a\n",
                    seed,rate,dt,tick,(unsigned long long)actual,(unsigned long long)expected,rest,port.carry,scale,port.intervalScale);return 1;
            }
            ++cases;
        }
    }
    std::printf("Original distance emission: %d consecutive steps, counts/carry/interval/RNG bit-identical.\n",cases);
}
