//此文件实现了 URLProtocol 抽象层广义文件操作函数，由于 URLProtocol 是底层其他具体文件(file,pipe 等)
// 的简单封装，这一层只是一个中转站，大部分函数都是简单中转到底层的具体实现函数。
#include "../berrno.h"
#include "avformat.h"

URLProtocol *first_protocol = NULL;

// ffplay 抽象底层的 file， pipe 等为 URLProtocol，然后把这些 URLProtocol 串联起来做成链表，便于查找。
// register_protocol 实际就是串联的各个 URLProtocol，全局表头为 first_protocol。
int register_protocol(URLProtocol *protocol)
{
    URLProtocol **p;
    p = &first_protocol;
    //移动指针到 URLProtocol 链表末尾。
    while (*p != NULL)
        p = &(*p)->next;
    *p = protocol;
    //在 URLProtocol 链表末尾直接挂接当前的 URLProtocol 指针。
    protocol->next = NULL;
    return 0;
}
/*
    打开广义输入文件。此函数主要有三部分逻辑，首先从文件路径名中分离出协议字符串到 proto_str
字符数组中， 接着遍历 URLProtocol 链表查找匹配 proto_str 字符数组中的字符串来确定使用的协议， 最
后调用相应的文件协议的打开函数打开输入文件。
*/
int url_open(URLContext **puc, const char *filename, int flags)
{
    URLContext *uc;
    URLProtocol *up;
    const char *p;
    char proto_str[128],  *q;
    int err;
//以冒号和结束符作为边界从文件名中分离出的协议字符串到 proto_str 字符数组。 由于协议只能是字符，
// 所以在边界前识别到非字符就断定是 file。
    p = filename;
    q = proto_str;
    while (*p != '\0' &&  *p != ':')
    {
        if (!isalpha(*p))  // protocols can only contain alphabetic chars
            goto file_proto;
        if ((q - proto_str) < sizeof(proto_str) - 1)
            *q++ =  *p;
        p++;
    }
    //如果协议字符串只有一个字符，我们就认为是 windows 下的逻辑盘符，断定是 file 。
    // if the protocol has length 1, we consider it is a dos drive
    if (*p == '\0' || (q - proto_str) <= 1)
    {
file_proto: 
		strcpy(proto_str, "file");
    }
    else
    {
        *q = '\0';
    }
    //遍历 URLProtocol 链表匹配使用的协议，如果没有找到就返回错误码。
    up = first_protocol;
    while (up != NULL)
    {
        if (!strcmp(proto_str, up->name))
            goto found;
        up = up->next;
    }
    err =  - ENOENT;
    goto fail;
    //如果找到就分配 URLContext 结构内存，特别注意内存大小要加上文件名长度，文件名字符串结束标
    //记 0 也要预先分配 1 个字节内存，这 1 个字节就是 URLContext 结构中的 char filename[1]。

found: 
	uc = av_malloc(sizeof(URLContext) + strlen(filename));
    if (!uc)
    {
        err =  - ENOMEM;
        goto fail;
    }
    //strcpy 函数会自动在 filename 字符数组后面补 0 作为字符串结束标记，所以不用特别赋值为 0。
    strcpy(uc->filename, filename);
    uc->prot = up;
    uc->flags = flags;
    uc->max_packet_size = 0; // default: stream file
    //接着调用相应协议的文件打开函数实质打开文件。如果文件打开错误，就需要释放 malloc 出来的内
    //存，并返回错误码。
    err = up->url_open(uc, filename, flags);
    if (err < 0)
    {
        av_free(uc);        //打开失败 释放内存
        *puc = NULL;
        return err;
    }
    *puc = uc;
    return 0;
fail:
	*puc = NULL;
    return err;
}

//读操作
int url_read(URLContext *h, unsigned char *buf, int size)
{
    int ret;
    if (h->flags &URL_WRONLY)
        return AVERROR_IO;
    ret = h->prot->url_read(h, buf, size);
    return ret;
}
//seek操作
offset_t url_seek(URLContext *h, offset_t pos, int whence)
{
    offset_t ret;

    if (!h->prot->url_seek)
        return  - EPIPE;
    ret = h->prot->url_seek(h, pos, whence);
    return ret;
}
//关闭操作 释放内存
int url_close(URLContext *h)
{
    int ret;

    ret = h->prot->url_close(h);
    av_free(h);
    return ret;
}
//读取数据包大小如果非0必须是实质有效的
int url_get_max_packet_size(URLContext *h)
{
    return h->max_packet_size;
}
