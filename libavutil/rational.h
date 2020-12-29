#ifndef RATIONAL_H
#define RATIONAL_H

typedef struct AVRational
{
    int num; // numerator   // 分母
    int den; // denominator // 分子
} AVRational;

static inline double av_q2d(AVRational a)
{
    return a.num / (double)a.den;
}

#endif
