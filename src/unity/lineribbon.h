// SPDX-License-Identifier: GPL-3.0-or-later
// FX_Touch's planar, TransformZ-aligned TrailRenderer path. Native references:
// 399ed0 / 39b440 (normals/miter), 392cb0 (rounded joins), 3916b0 (caps).
#pragma once
#include "circle.h"
#include <span>
#include <vector>
#include <bit>

namespace baclickfx::unity {
using LinePoint = std::array<float,2>;
inline float lineMul(float a,float b){volatile float v=a*b;return v;}
inline float lineAdd(float a,float b){volatile float v=a+b;return v;}
inline float lineSub(float a,float b){volatile float v=a-b;return v;}
inline float lineSquareLength(LinePoint p){return lineAdd(lineMul(p[0],p[0]),lineMul(p[1],p[1]));}
inline LinePoint lineNormalize(LinePoint p,LinePoint fallback={1,0}) {
    if(!(lineSquareLength(p)>1.0000000031710769e-30f))return fallback;
    const auto v=shapeNormalize({p[0],p[1],0});return {v[0],v[1]};
}
inline LinePoint lineMiter(LinePoint before,LinePoint after,float maxWidth) {
    const auto squareDelta=lineSquareLength({lineSub(before[0],after[0]),lineSub(before[1],after[1])});
    const auto squareSum=lineSquareLength({lineAdd(before[0],after[0]),lineAdd(before[1],after[1])});
    const float blend=std::min(1.f,std::min(lineMul(squareDelta,50.f),lineMul(squareSum,50.f)));
    volatile float a=before[0]/-before[1], b=-after[0]/after[1];
    const float first=std::clamp(float(a),-100000.f,100000.f),second=std::clamp(float(b),-100000.f,100000.f);
    const float intercept1=lineSub(before[1],lineMul(first,before[0]));
    const float intercept2=lineSub(after[1],lineMul(second,after[0]));
    LinePoint result=before;
    const float difference=lineSub(first,second);
    if(std::abs(difference)>=9.999999747378752e-6f) {
        volatile float x=lineSub(intercept2,intercept1)/difference;
        const float y=lineAdd(lineMul(first,x),intercept1);
        result={lineAdd(before[0],lineMul(lineSub(x,before[0]),blend)),
                lineAdd(before[1],lineMul(lineSub(y,before[1]),blend))};
    }
    const float limit=std::max(1.f,lineMul(3.f,maxWidth));
    if(lineSquareLength(result)>lineMul(limit,limit)) {
        result=lineNormalize(result);for(auto &v:result)v=lineMul(v,limit);
    }
    return result;
}
inline float lineAtan2(float y,float x) {
    volatile float ratio=y/x;
    const float absolute=std::abs(float(ratio));
    const float r=absolute>1.f?shapeReciprocal(absolute):absolute;
    const float squared=lineMul(r,r),fourth=lineMul(squared,squared),sixth=lineMul(fourth,squared);
    const float eighth=lineMul(sixth,squared),tenth=lineMul(eighth,squared),twelfth=lineMul(tenth,squared);
    float poly=lineAdd(lineMul(-.3332645893096924f,squared),.999999463558197f);
    poly=lineAdd(poly,lineMul(.1988152414560318f,fourth));
    poly=lineAdd(poly,lineMul(-.1348726600408554f,sixth));
    poly=lineAdd(poly,lineMul(.08387135714292526f,eighth));
    poly=lineAdd(poly,lineMul(-.03701314330101013f,tenth));
    poly=lineAdd(poly,lineMul(.007862508296966553f,twelfth));
    float result=lineMul(poly,r);
    if(absolute>1.f)result=lineSub(1.5707963705062866f,result);
    if(std::signbit(x))result=lineSub(3.1415927410125732f,result);
    return std::copysign(result,y);
}
struct LineRibbonVertex {LinePoint position{};float u=0,v=0;std::size_t sample=0;};
struct LineRibbonScratch {
    std::vector<LinePoint> normals,miters;
    std::vector<float> lengths,ratios,progress;
};
// Input/output are head -> tail. Source outputs a triangle strip, including
// degenerate vertices between cap fans and the body; no overlapping corner fan.
inline void buildLineRibbon(std::span<const LinePoint> points,float width,int corners,int caps,
                            LineRibbonScratch &scratch,std::vector<LineRibbonVertex> &out) {
    out.clear();const auto count=points.size();if(count<2)return;
    auto &normals=scratch.normals;auto &lengths=scratch.lengths;auto &ratios=scratch.ratios;
    normals.resize(count);lengths.resize(count);ratios.resize(count);scratch.progress.resize(count);
    float total=0;LinePoint previousDirection{1,0};bool validDirection=false;
    const float inverseWidth=shapeReciprocal(lineMul(width,2.f));
    for(std::size_t i=0;i<count;++i) {
        const auto index=std::max(std::size_t(1),i);
        LinePoint delta{lineSub(points[index-1][0],points[index][0]),lineSub(points[index-1][1],points[index][1])};
        const float square=lineSquareLength(delta),length=std::sqrt(square);
        lengths[i]=i?length:0;total=lineAdd(total,lengths[i]);ratios[i]=lineMul(length,inverseWidth);
        const float threshold=count>2?9.999999747378752e-6f:1.0000000031710769e-30f;
        if(square<threshold) {
            if(index+1<count)delta={lineSub(points[index][0],points[index+1][0]),lineSub(points[index][1],points[index+1][1])};
            if(lineSquareLength(delta)<threshold)delta=validDirection?previousDirection:LinePoint{1,0};
            else {previousDirection=delta;validDirection=true;}
        }else {previousDirection=delta;validDirection=true;}
        normals[i]=lineNormalize({delta[1],-delta[0]});
        if(i && length<lineMul(width,2.f)) {
            const auto before=normals[i-1],after=normals[i];
            const float blend=lineMul(std::max(0.f,lineSub(ratios[i],.5f)),2.f);
            const LinePoint mean{lineMul(lineAdd(before[0],after[0]),.5f),lineMul(lineAdd(before[1],after[1]),.5f)};
            normals[i-1]=lineNormalize({lineAdd(mean[0],lineMul(lineSub(before[0],mean[0]),blend)),
                lineAdd(mean[1],lineMul(lineSub(before[1],mean[1]),blend))},before);
            normals[i]=lineNormalize({lineAdd(mean[0],lineMul(lineSub(after[0],mean[0]),blend)),
                lineAdd(mean[1],lineMul(lineSub(after[1],mean[1]),blend))},after);
        }
    }
    const float inverseLength=shapeReciprocal(std::max(total,9.999999974752427e-7f));
    float travelled=0;for(std::size_t i=0;i<count;++i){travelled=lineAdd(travelled,lengths[i]);scratch.progress[i]=lineMul(travelled,inverseLength);}
    const float half=lineMul(width,.5f);
    const auto vertex=[&](std::size_t i,LinePoint p,float v){out.push_back({p,scratch.progress[i],v,i});};
    const auto cap=[&](std::size_t i,LinePoint normal,bool end) {
        const int segments=caps+1;const float inverse=shapeReciprocal(float(segments));
        const float step=lineMul(end?-3.1415927410125732f:3.1415927410125732f,inverse);
        float angle=0;
        for(int j=0;j<=segments;++j) {
            const auto trig=shapeCosSin(lineMul(angle,.15915493667125702f));
            const LinePoint offset{lineSub(lineMul(normal[0],trig[0]),lineMul(normal[1],trig[1])),
                lineAdd(lineMul(normal[0],trig[1]),lineMul(normal[1],trig[0]))};
            const LinePoint edge{lineAdd(points[i][0],lineMul(offset[0],half)),lineAdd(points[i][1],lineMul(offset[1],half))};
            if(end){vertex(i,points[i],.5f);vertex(i,edge,lineMul(float(j),inverse));}
            else {vertex(i,edge,lineMul(float(j),inverse));vertex(i,points[i],.5f);}
            angle=lineAdd(angle,step);
        }
    };
    for(std::size_t i=0;i<count;++i) {
        if(i>0 && i+1<count && corners>0) {
            const auto before=normals[i],after=normals[i+1];
            const float start=lineAtan2(before[1],before[0]);
            float angle=lineSub(lineAtan2(after[1],after[0]),start);
            if(angle< -3.1415927410125732f)angle=lineAdd(angle,6.2831854820251465f);
            if(angle>3.1415927410125732f)angle=lineSub(angle,6.2831854820251465f);
            const float sign=std::signbit(angle)?1.f:-1.f;
            const auto miter=lineMiter({-before[0],-before[1]},{-after[0],-after[1]},std::min(ratios[i-1],1.f));
            const LinePoint anchor{lineAdd(points[i][0],lineMul(lineMul(miter[0],sign),half)),
                lineAdd(points[i][1],lineMul(lineMul(miter[1],sign),half))};
            const float radius=lineMul(sign,lineMul(half,2.f));
            const float step=lineMul(angle,shapeReciprocal(float(corners)));float current=0;
            for(int j=0;j<=corners;++j) {
                const auto trig=shapeCosSin(lineMul(current,.15915493667125702f));
                const LinePoint normal{lineSub(lineMul(before[0],trig[0]),lineMul(before[1],trig[1])),
                    lineAdd(lineMul(before[0],trig[1]),lineMul(before[1],trig[0]))};
                const LinePoint edge{lineAdd(anchor[0],lineMul(normal[0],radius)),lineAdd(anchor[1],lineMul(normal[1],radius))};
                if(sign<0){vertex(i,edge,1);vertex(i,anchor,0);}else{vertex(i,anchor,1);vertex(i,edge,0);}
                current=lineAdd(current,step);
            }
        } else {
            const auto normal=normals[std::min(i+1,count-1)];
            if(i==0 && caps>0)cap(i,normal,false);
            vertex(i,{lineSub(points[i][0],lineMul(normal[0],half)),lineSub(points[i][1],lineMul(normal[1],half))},1);
            vertex(i,{lineAdd(points[i][0],lineMul(normal[0],half)),lineAdd(points[i][1],lineMul(normal[1],half))},0);
            if(i+1==count && caps>0)cap(i,normal,true);
        }
    }
}
}
