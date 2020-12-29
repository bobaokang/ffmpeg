#include "avformat.h"

extern URLProtocol file_protocol;
//编程基本原则之一，初始化函数只调用一次，不能随意多次调用。
//7 到 11 行， inited 变量声明成 static，做一下比较是为了避免此函数多次调用。

void av_register_all(void)
{
    static int inited = 0;

    if (inited != 0)    //避免被多次定义
        return ;
    inited = 1;

    //ffplay 把 CPU 当做一个广义的 DSP。有些计算可以用 CPU 自带的加速指令来优化， ffplay 把这类函数
    //独立出来放到 dsputil.h 和 dsputil.c 文件中， 用函数指针的方法映射到各个 CPU 具体的加速优化实现函数，
    //此处初始化这些函数指针。
    avcodec_init();

    // 把所有的解码器用链表的方式都串连起来，链表头指针是 first_avcodec。
    avcodec_register_all();
    //把所有的输入文件格式用链表的方式都串连起来，链表头指针是 first_iformat。
    avidec_init();
    //把所有的输入协议用链表的方式都串连起来，比如 tcp/udp/file 等，链表头指针是 first_protocol。
    register_protocol(&file_protocol);
}
