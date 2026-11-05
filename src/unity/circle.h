// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from kde-shittim-lockscreen: fx/particles/ShittimParticleCircle.h.
// Native UnityPlayer reference and scope: docs/unity-parity.md.
#pragma once

/**
 * @file ShittimParticleCircle.h
 * @brief 按源四 lane 随机流采样圆形发射器的位置和方向。
 */
#include "shape.h"

namespace baclickfx::unity {
inline float shapeSinPolynomial(float x) {
    volatile float square=x*x,fourth=square*square,eighth=fourth*fourth;
    volatile float a=-41.34165954589844f*square,b=-76.56861877441406f*square;
    a+=6.283185005187988f;b+=81.60182189941406f;
    b*=fourth;b+=a;
    volatile float last=39.65703201293945f*eighth;b+=last;
    return b*x;
}
// 源 SIMD 三角运算使用圈数和最近偶数范围归约，不替换为 libm。
inline std::array<float,2> shapeCosSin(float turns) {
    volatile float cosFraction=turns-float(std::nearbyint(turns));
    volatile float shifted=turns-.25f;
    volatile float sinFraction=shifted-float(std::nearbyint(float(shifted)));
    return {shapeSinPolynomial(.25f-std::abs(float(cosFraction))),
            shapeSinPolynomial(.25f-std::abs(float(sinFraction)))};
}
inline float shapeReciprocal(float value) {
#if defined(__SSE__)
    const float initial=_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(value)));
    volatile float a=initial*value,b=2.000000476837158f-a;
    volatile float first=b*initial,c=first*value,d=2.f-c;
    const float result=first*d;
    return std::isnan(result)?initial:result;
#else
    return 1.f/value;
#endif
}
struct CircleShapeConfig {
    ShapeVector scale{1,1,1},position{},transformScale{1,1,1};
    float rotationZ=0,radius=1,thickness=1,arc=360,spread=0;
    float randomDirection=0,sphericalDirection=0;
    int arcMode=0;
};
class CircleShape {
public:
    explicit CircleShape(std::uint32_t seed=0) {
        for(std::uint32_t lane=0;lane<4;++lane)m_streams[lane].init(seed+367u*lane);
    }
    void beginEmission(std::uint64_t count) {m_cursor=4;m_index=0;m_count=count;}
    std::array<std::uint32_t,16> state() const {
        std::array<std::uint32_t,16> result{};
        for(int lane=0;lane<4;++lane)for(int word=0;word<4;++word)
            result[size_t(word*4+lane)]=m_streams[size_t(lane)].state()[size_t(word)];
        return result;
    }
    ShapeSample next(const CircleShapeConfig &c) {
        if(m_cursor==4)batch(c);
        return m_samples[size_t(m_cursor++)];
    }
private:
    static ShapeVector randomDirection(Random &rng) {
        volatile float angle=rng.value()*6.2831854820251465f;
        volatile float turns=angle*.15915493667125702f;
        volatile float z=rng.value()*2.f;z-=1.f;
        volatile float squared=z*z,radial=std::sqrt(1.f-squared);
        const auto trig=shapeCosSin(turns);
        return {trig[0]*radial,trig[1]*radial,z};
    }
    static void mix(ShapeVector &direction,const ShapeVector &other,float amount) {
        for(int axis=0;axis<3;++axis) {
            volatile float delta=other[size_t(axis)]-direction[size_t(axis)];
            volatile float product=delta*amount;direction[size_t(axis)]+=product;
        }
    }
    static ShapeSample transform(const CircleShapeConfig &c,const ShapeVector &p,const ShapeVector &d) {
        volatile float radians=c.rotationZ*.01745329238474369f,half=radians*.5f,turns=half*.15915493667125702f;
        const auto trig=shapeCosSin(turns);
        volatile float twiceZ=2.f*trig[1],diagonalProduct=-twiceZ*trig[1],offDiagonalProduct=(2.f*trig[0])*trig[1];
        const float diagonal=1.f+diagonalProduct;
        const std::array<ShapeVector,3> rotation{{{diagonal,-offDiagonalProduct,0},{offDiagonalProduct,diagonal,0},{0,0,1}}};
        std::array<ShapeVector,3> matrix{};
        for(int row=0;row<3;++row)for(int col=0;col<3;++col) {
            volatile float local=rotation[size_t(row)][size_t(col)]*c.scale[size_t(col)];
            matrix[size_t(row)][size_t(col)]=local*c.transformScale[size_t(row)];
        }
        ShapeSample result;ShapeVector direction{};
        for(int row=0;row<3;++row) {
            const auto &m=matrix[size_t(row)];
            volatile float a=m[0]*p[0],b=m[1]*p[1],z=m[2]*p[2];
            volatile float translation=c.position[size_t(row)]*c.transformScale[size_t(row)],tail=z+translation;
            tail=b+tail;result.position[size_t(row)]=a+tail;
            volatile float da=m[0]*d[0],db=m[1]*d[1],dz=m[2]*d[2],dtail=db+dz;
            direction[size_t(row)]=da+dtail;
        }
        result.direction=shapeNormalize(direction,true);return result;
    }
    void batch(const CircleShapeConfig &c) {
        volatile float arc=c.arc*.01745329238474369f,spread=c.spread*arc;
        volatile float inner=1.f-c.thickness;inner*=inner;
        volatile float outer=1.f-inner;
        const std::uint64_t divisor=c.arc==360.f?m_count:std::max<std::uint64_t>(m_count>0?m_count-1:0,1);
        const float reciprocal=c.arcMode==3?shapeReciprocal(float(divisor)):0;
        for(int lane=0;lane<4;++lane) {
            auto &rng=m_streams[size_t(lane)];float angle=0;
            if(c.arcMode==3) {
                volatile float fraction=reciprocal*float(m_index+std::uint64_t(lane));angle=fraction*arc;
                if(spread>0) {volatile float steps=angle/spread;angle=std::floor(steps)*spread;}
            } else if(spread>0) {
                volatile float steps=std::ceil(arc/spread),total=spread*steps,raw=rng.value()*total;
                volatile float fraction=raw/spread;angle=std::floor(fraction)*spread;
            } else angle=rng.value()*arc;
            volatile float turns=angle*.15915493667125702f;
            volatile float radialProduct=outer*rng.value(),radius=std::sqrt(radialProduct+inner);radius*=c.radius;
            const auto trig=shapeCosSin(turns);
            const ShapeVector p{trig[0]*radius,trig[1]*radius,0};
            auto direction=shapeNormalize({trig[0],trig[1],0});
            if(c.randomDirection>0)mix(direction,randomDirection(rng),c.randomDirection);
            if(c.sphericalDirection>0)mix(direction,shapeNormalize(p),c.sphericalDirection);
            m_samples[size_t(lane)]=transform(c,p,direction);
        }
        m_index+=4;m_cursor=0;
    }
    std::array<Random,4> m_streams;
    std::array<ShapeSample,4> m_samples{};
    int m_cursor=4;
    std::uint64_t m_index=0,m_count=1;
};
}
