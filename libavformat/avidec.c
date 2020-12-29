/*
AVI 文件解析的相关函数，注意有些地方有些技巧性代码。
注意 1： AVI 文件容器媒体数据有两种存放方式，非交织存放和交织存放。交织存放就是音视频数据以帧为最
小连续单位，相互间隔存放，这样音视频帧互相交织在一起，并且存放的间隔没有特别规定；非交织存放就是把
单一媒体的所有数据帧连续存放在一起，非交织存放的 avi 文件很少。
注意 2： AVI 文件索引结构 AVIINDEXENTRY 中的 dwChunkOffset 字段指示的偏移有的是相对文件开始字节的偏
移，有的事相对文件数据块 chunk 的偏移。
注意 3：附带的 avi 测试文件是交织存放的。
*/
#include "avformat.h"

#include <assert.h>

#define AVIIF_INDEX			0x10

#define AVIF_HASINDEX		0x00000010	// Index at end of file?
#define AVIF_MUSTUSEINDEX	0x00000020

#define INT_MAX	2147483647

#define MKTAG(a,b,c,d) (a | (b << 8) | (c << 16) | (d << 24))

#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))

static int avi_load_index(AVFormatContext *s);
static int guess_ni_flag(AVFormatContext *s);

//AVI 文件中的流参数定义，和 AVStream 数据结构协作。
typedef struct
{
    // // 帧偏移，视频用帧计数，音频用字节计数，用于计算 pts 表示时间
    int64_t frame_offset; // current frame(video) or byte(audio) counter(used to compute the pts)
    int remaining;  /// 表示需要读的数据大小，初值是帧裸数组大小，全部读完后为 0
    int packet_size;    //大小，非交织和帧裸数据大小相同，交织比帧裸数据大小大 8 字节

    int scale;
    int rate;
    int sample_size; // size of one sample (or packet) (in the rate/scale sense) in bytes

    int64_t cum_len; // temporary storage (used during seek)

    int prefix;      // normally 'd'<<8 + 'c' or 'w'<<8 + 'b'
    int prefix_count;
} AVIStream;

//AVI 文件中的文件格式参数相关定义，和 AVFormatContext 协作。
typedef struct
{
    int64_t riff_end;    // RIFF块大小
    int64_t movi_list;   /// 媒体数据块开始字节相对文件开始字节的偏移
    int64_t movi_end;    /// 媒体数据块开始字节相对文件开始字节的偏移
    int non_interleaved;    //指示是否是非交织AVI
    int stream_index_2;  // 为了和AVPacket中的stream_index相区别
} AVIContext;           //示当前应该读取的流的索引。初值为-1，表示没有确定应该读的流。
                        // 实际表示 AVFormatContext 结构中 AVStream *streams[]数组中的索引

//CodecTag 数据结构，用于关联具体媒体格式的 ID 和 Tag 标签。
typedef struct
{
    int id;
    unsigned int tag;
} CodecTag;

//瘦身后的 ffplay 支持的一些视频媒体 ID 和 Tag 标签数组。
const CodecTag codec_bmp_tags[] =
{
    {CODEC_ID_MSRLE, MKTAG('m', 'r', 'l', 'e')},
    {CODEC_ID_MSRLE, MKTAG(0x1, 0x0, 0x0, 0x0)},
    {CODEC_ID_NONE,  0},
};

// 瘦身后的 ffplay 支持的一些音频媒体 ID 和 Tag 标签数组。
const CodecTag codec_wav_tags[] =
{
    {CODEC_ID_TRUESPEECH, 0x22},
    {0, 0},
};

//以媒体 tag 标签为关键字，查找 codec_bmp_tags 或 codec_wav_tags 数组，返回媒体 ID。
enum CodecID codec_get_id(const CodecTag *tags, unsigned int tag)
{
    while (tags->id != CODEC_ID_NONE)
    {
        //比较 Tag 关键字，相等时返回对应媒体 ID。
        if (toupper((tag >> 0) &0xFF) == toupper((tags->tag >> 0) &0xFF) 
         && toupper((tag >> 8) &0xFF) == toupper((tags->tag >> 8) &0xFF) 
         && toupper((tag >> 16)&0xFF) == toupper((tags->tag >> 16)&0xFF) 
         && toupper((tag >> 24)&0xFF) == toupper((tags->tag >> 24)&0xFF))
            return tags->id;
        //比较 Tag 关键字，不等移到数组的下一项。
        tags++;
    }
    //所有关键字都不匹配，返回 CODEC_ID_NONE。
    return CODEC_ID_NONE;
}
//校验 AVI 文件，读取 AVI 文件媒体数据块的偏移大小信息， 和 avi_probe()函数部分相同。
static int get_riff(AVIContext *avi, ByteIOContext *pb) 
{
    uint32_t tag;
    tag = get_le32(pb);
    //校验 AVI 文件开始关键字串"RIFF"。
    if (tag != MKTAG('R', 'I', 'F', 'F'))
        return  - 1;

    avi->riff_end = get_le32(pb); // RIFF chunk size
    avi->riff_end += url_ftell(pb); // RIFF chunk end
    tag = get_le32(pb);
    //校验 AVI 文件关键字串"AVI "或"AVIX"。
    if (tag != MKTAG('A', 'V', 'I', ' ') && tag != MKTAG('A', 'V', 'I', 'X'))
        return  - 1;
    //如果通过 AVI 文件关键字串"RIFF"和"AVI "或"AVIX"校验，就认为是 AVI 文件，这种方式非常可靠。
    return 0;
}
// 排序建立 AVI 索引表，函数名为 clean_index,不准确，功能以具体的实现代码为准。
static void clean_index(AVFormatContext *s)
{
    int i, j;

    for (i = 0; i < s->nb_streams; i++)
    {
        //对每个流都建一个独立的索引表。
        AVStream *st = s->streams[i];
        AVIStream *ast = st->priv_data;
        int n = st->nb_index_entries;
        int max = ast->sample_size;
        int64_t pos, size, ts;
        //如果索引表项大于 1，则认为索引表已建好，不再排序重建。如果 sample_siz e 为 0,则没办法重建。
        if (n != 1 || ast->sample_size == 0)
            continue;

        /*
            此种情况多半是用在非交织存储的 avi 音频流。不管交织还是非交织存储，视频流通常都有索引。
        防止包太小需要太多的索引项占有大量内存，设定最小帧 siz e 阈值为 1024。比如有些音频流，最小解
        码帧只十多个字节， 如果文件比较大则在索引上耗费太多内存
        */
        while (max < 1024)
            max += max;
        //取位置，大小，时钟等基本参数。
        pos = st->index_entries[0].pos;
        size = st->index_entries[0].size;
        ts = st->index_entries[0].timestamp;

        for (j = 0; j < size; j += max)
        {
            //以 max 指定的字节打包成帧，添加到索引表。
            av_add_index_entry(st, pos + j, ts + j / ast->sample_size, FFMIN(max, size - j), 0, AVINDEX_KEYFRAME);
        }
    }
}
//读取 AVI 文件头，读取 AVI 文件索引，并识别具体的媒体格式，关联一些数据结构。
static int avi_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    AVIContext *avi = s->priv_data;
    ByteIOContext *pb = &s->pb;
    uint32_t tag, tag1, handler;
    int codec_type, stream_index, frame_period, bit_rate;
    unsigned int size, nb_frames;
    int i, n;
    AVStream *st;
    AVIStream *ast;
    //当前应该读取的流的索引赋初值为-1，表示没有确定应该读的流。
    avi->stream_index_2 =  - 1;
    //校验 AVI 文件，读取 AVI 文件媒体数据块的偏移大小信息。
    if (get_riff(avi, pb) < 0)
        return  - 1;
    //简单变量符初值。
    stream_index =  - 1; // first list tag
    codec_type =  - 1;
    frame_period = 0;

    for (;;)
    {
        //AVI 文件的基本结构是块，一个文件有多个块，并且块还可以内嵌，在这里循环读文件头中的块。
        if (url_feof(pb))
            goto fail;
            //读取每个块的标签和大小。
        tag = get_le32(pb);
        size = get_le32(pb);
        switch (tag)
        {
        case MKTAG('L', 'I', 'S', 'T'):  // ignored, except when start of video packets
            tag1 = get_le32(pb);
            if (tag1 == MKTAG('m', 'o', 'v', 'i'))
            {
                //读取 movi 媒体数据块的偏移和大小。
                avi->movi_list = url_ftell(pb) - 4;
                if (size)
                    avi->movi_end = avi->movi_list + size;
                else
                    avi->movi_end = url_fsize(pb);

                goto end_of_header; // 读到数据段就认为文件头结束了，就goto
            }
            break;
        case MKTAG('a', 'v', 'i', 'h'):  // avi header, using frame_period is bad idea
            frame_period = get_le32(pb);
            bit_rate = get_le32(pb) *8;
            get_le32(pb);
            avi->non_interleaved |= get_le32(pb) &AVIF_MUSTUSEINDEX;

            url_fskip(pb, 2 *4);
            n = get_le32(pb);
            for (i = 0; i < n; i++)
            {
                AVIStream *ast;
                st = av_new_stream(s, i);
                if (!st)
                    goto fail;

                ast = av_mallocz(sizeof(AVIStream));
                if (!ast)
                    goto fail;
                st->priv_data = ast;

                st->actx->bit_rate = bit_rate;
            }
            url_fskip(pb, size - 7 * 4);
            break;
        case MKTAG('s', 't', 'r', 'h'):  // stream header
            stream_index++;
            tag1 = get_le32(pb);
            handler = get_le32(pb);

            if (stream_index >= s->nb_streams)
            {
                url_fskip(pb, size - 8);
                break;
            }
            st = s->streams[stream_index];
            ast = st->priv_data;

            get_le32(pb); // flags
            get_le16(pb); // priority
            get_le16(pb); // language
            get_le32(pb); // initial frame
            ast->scale = get_le32(pb);
            ast->rate = get_le32(pb);
            if (ast->scale && ast->rate)
            {}
            else if (frame_period)
            {
                ast->rate = 1000000;
                ast->scale = frame_period;
            }
            else
            {
                ast->rate = 25;
                ast->scale = 1;
            }
            av_set_pts_info(st, 64, ast->scale, ast->rate);

            ast->cum_len = get_le32(pb); // start
            nb_frames = get_le32(pb);

            get_le32(pb); // buffer size
            get_le32(pb); // quality
            ast->sample_size = get_le32(pb); // sample ssize

            switch (tag1)
            {
            case MKTAG('v', 'i', 'd', 's'): codec_type = CODEC_TYPE_VIDEO;
                ast->sample_size = 0;
                break;
            case MKTAG('a', 'u', 'd', 's'): codec_type = CODEC_TYPE_AUDIO;
                break;
            case MKTAG('t', 'x', 't', 's'):  //FIXME
                codec_type = CODEC_TYPE_DATA; //CODEC_TYPE_SUB ?  FIXME
                break;
            case MKTAG('p', 'a', 'd', 's'): codec_type = CODEC_TYPE_UNKNOWN;
                stream_index--;
                break;
            default:
                goto fail;
            }
            ast->frame_offset = ast->cum_len *FFMAX(ast->sample_size, 1);
            url_fskip(pb, size - 12 * 4);
            break;
        case MKTAG('s', 't', 'r', 'f'):  // stream header
            if (stream_index >= s->nb_streams)
            {
                url_fskip(pb, size);
            }
            else
            {
                st = s->streams[stream_index];
                switch (codec_type)
                {
                case CODEC_TYPE_VIDEO:    // BITMAPINFOHEADER
                    get_le32(pb); // size
                    st->actx->width = get_le32(pb);
                    st->actx->height = get_le32(pb);
                    get_le16(pb); // panes
                    st->actx->bits_per_sample = get_le16(pb); // depth
                    tag1 = get_le32(pb);
                    get_le32(pb); // ImageSize
                    get_le32(pb); // XPelsPerMeter
                    get_le32(pb); // YPelsPerMeter
                    get_le32(pb); // ClrUsed
                    get_le32(pb); // ClrImportant

                    if (size > 10 *4 && size < (1 << 30))
                    {
                        st->actx->extradata_size = size - 10 * 4;
                        st->actx->extradata = av_malloc(st->actx->extradata_size + 
							                             FF_INPUT_BUFFER_PADDING_SIZE);
                        url_fread(pb, st->actx->extradata, st->actx->extradata_size);
                    }

                    if (st->actx->extradata_size &1)
                        get_byte(pb);

                    /* Extract palette from extradata if bpp <= 8 */
                    /* This code assumes that extradata contains only palette */
                    /* This is true for all paletted codecs implemented in ffmpeg */
                    if (st->actx->extradata_size && (st->actx->bits_per_sample <= 8))
                    {
                        int min = FFMIN(st->actx->extradata_size, AVPALETTE_SIZE);

                        st->actx->palctrl = av_mallocz(sizeof(AVPaletteControl));
                        memcpy(st->actx->palctrl->palette, st->actx->extradata, min);
                        st->actx->palctrl->palette_changed = 1;
                    }

                    st->actx->codec_type = CODEC_TYPE_VIDEO;
                    st->actx->codec_id = codec_get_id(codec_bmp_tags, tag1);

                    st->frame_last_delay = 1.0 * ast->scale / ast->rate;

                    break;
                 case CODEC_TYPE_AUDIO:
                    {
                        AVCodecContext *actx = st->actx;

                        int id = get_le16(pb);
                        actx->codec_type = CODEC_TYPE_AUDIO;
                        actx->channels = get_le16(pb);
                        actx->sample_rate = get_le32(pb);
                        actx->bit_rate = get_le32(pb) *8;
                        actx->block_align = get_le16(pb);
                        if (size == 14)  // We're dealing with plain vanilla WAVEFORMAT
                            actx->bits_per_sample = 8;
                        else
                            actx->bits_per_sample = get_le16(pb);
                        actx->codec_id = codec_get_id(codec_wav_tags, id); // wav_codec_get_id(id, codec->bits_per_sample);						

                        if (size > 16)
                        {
                            actx->extradata_size = get_le16(pb); // We're obviously dealing with WAVEFORMATEX
                            if (actx->extradata_size > 0)
                            {
                                if (actx->extradata_size > size - 18)
                                    actx->extradata_size = size - 18;
                                actx->extradata = av_mallocz(actx->extradata_size + 
                                                         FF_INPUT_BUFFER_PADDING_SIZE);
                                url_fread(pb, actx->extradata, actx->extradata_size);
                            }
                            else
                            {
                                actx->extradata_size = 0;
                            }

                            // It is possible for the chunk to contain garbage at the end
                            if (size - actx->extradata_size - 18 > 0)
                                url_fskip(pb, size - actx->extradata_size - 18);
                        }
                    }

                    if (size % 2) // 2-aligned (fix for Stargate SG-1 - 3x18 - Shades of Grey.avi)
                        url_fskip(pb, 1);

                    break;
                 default:
                    st->actx->codec_type = CODEC_TYPE_DATA;
                    st->actx->codec_id = CODEC_ID_NONE;
                    url_fskip(pb, size);
                    break;
                }
            }
            break;
        default:  // skip tag
            size += (size &1);
            url_fskip(pb, size);
            break;
        }
    }

end_of_header: 
    if (stream_index != s->nb_streams - 1) // check stream number
    {
fail: 
        for (i = 0; i < s->nb_streams; i++)
        {
            av_freep(&s->streams[i]->actx->extradata);
            av_freep(&s->streams[i]);
        }
        return  - 1;
    }

    avi_load_index(s);

    avi->non_interleaved |= guess_ni_flag(s);
    if (avi->non_interleaved) 
        clean_index(s);

    return 0;
}

int avi_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIContext *avi = s->priv_data;
    ByteIOContext *pb = &s->pb;
    int n, d[8], size;
    offset_t i, sync;

    if (avi->non_interleaved)
    {
        int best_stream_index = 0;
        AVStream *best_st = NULL;
        AVIStream *best_ast;
        int64_t best_ts = INT64_MAX;
        int i;

        for (i = 0; i < s->nb_streams; i++)
        {
            AVStream *st = s->streams[i];
            AVIStream *ast = st->priv_data;
            int64_t ts = ast->frame_offset;

            if (ast->sample_size)
                ts /= ast->sample_size;

            ts = av_rescale(ts, AV_TIME_BASE *(int64_t)st->time_base.num, st->time_base.den);

            if (ts < best_ts)
            {
                best_ts = ts;
                best_st = st;
                best_stream_index = i;
            }
        }
        best_ast = best_st->priv_data;
        best_ts = av_rescale(best_ts, best_st->time_base.den, AV_TIME_BASE *(int64_t)best_st->time_base.num);
        if (best_ast->remaining)
            i = av_index_search_timestamp(best_st, best_ts, AVSEEK_FLAG_ANY | AVSEEK_FLAG_BACKWARD);
        else
            i = av_index_search_timestamp(best_st, best_ts, AVSEEK_FLAG_ANY);

        if (i >= 0)     
        {
            int64_t pos = best_st->index_entries[i].pos;
            pos += best_ast->packet_size - best_ast->remaining;
            url_fseek(&s->pb, pos + 8, SEEK_SET);

            assert(best_ast->remaining <= best_ast->packet_size);

            avi->stream_index_2 = best_stream_index;
            if (!best_ast->remaining)
                best_ast->packet_size = best_ast->remaining = best_st->index_entries[i].size;
        }
    }

resync:

    if (avi->stream_index_2 >= 0)
    {
        AVStream *st = s->streams[avi->stream_index_2];
        AVIStream *ast = st->priv_data;
        int size;

        if (ast->sample_size <= 1) // minorityreport.AVI block_align=1024 sample_size=1 IMA-ADPCM
            size = INT_MAX;
        else if (ast->sample_size < 32)
            size = 64 * ast->sample_size;
        else
            size = ast->sample_size;

        if (size > ast->remaining)
            size = ast->remaining;

        av_get_packet(pb, pkt, size);

        pkt->dts = ast->frame_offset;

        if (ast->sample_size)
            pkt->dts /= ast->sample_size;

        pkt->stream_index = avi->stream_index_2;

        if (st->actx->codec_type == CODEC_TYPE_VIDEO)
        {
            if (st->index_entries)
            {
                AVIndexEntry *e;
                int index;

                index = av_index_search_timestamp(st, pkt->dts, 0);
                e = &st->index_entries[index];

                if (index >= 0 && e->timestamp == ast->frame_offset)
                {
                    if (e->flags &AVINDEX_KEYFRAME)
                        pkt->flags |= PKT_FLAG_KEY;
                }
            }
            else
            {
                pkt->flags |= PKT_FLAG_KEY; // if no index, better to say that all frames are key frames
            }
        }
        else
        {
            pkt->flags |= PKT_FLAG_KEY;
        }

        if (ast->sample_size)
            ast->frame_offset += pkt->size;
        else
            ast->frame_offset++;

        ast->remaining -= size;
        if (!ast->remaining)
        {
            avi->stream_index_2 =  - 1;
            ast->packet_size = 0;
            if (size &1)
            {
                get_byte(pb);
                size++;
            }
        }

        return size;
    }

    memset(d,  - 1, sizeof(int) *8);
    for (i = sync = url_ftell(pb); !url_feof(pb); i++)
    {
        int j;

        if (i >= avi->movi_end)
            break;

        for (j = 0; j < 7; j++)
            d[j] = d[j + 1];

        d[7] = get_byte(pb);

        size = d[4] + (d[5] << 8) + (d[6] << 16) + (d[7] << 24);

        if (d[2] >= '0' && d[2] <= '9' && d[3] >= '0' && d[3] <= '9')
        {
            n = (d[2] - '0') *10+(d[3] - '0');
        }
        else
        {
            n = 100; //invalid stream id
        }

        if (i + size > avi->movi_end || d[0] < 0)
            continue;

        if ((d[0] == 'i' && d[1] == 'x' && n < s->nb_streams) 
         || (d[0] == 'J' && d[1] == 'U' && d[2] == 'N' && d[3] == 'K'))
        {
            url_fskip(pb, size);
            goto resync;
        }

        if (d[0] >= '0' && d[0] <= '9' && d[1] >= '0' && d[1] <= '9')
        {
            n = (d[0] - '0') *10+(d[1] - '0');
        }
        else
        {
            n = 100; //invalid stream id
        }

        //parse ##dc/##wb
        if (n < s->nb_streams)
        {
            AVStream *st;
            AVIStream *ast;
            st = s->streams[n];
            ast = st->priv_data;

			if(sync + 9 <= i)
			{
				int dbg=0;
			}
			else
			{
				int dbg1=0;
			}

            if (((ast->prefix_count < 5 || sync + 9 > i) && d[2] < 128 && d[3] < 128)
            	   || d[2] * 256 + d[3] == ast->prefix)
            {
                if (d[2] * 256 + d[3] == ast->prefix)
                    ast->prefix_count++;
                else
                {
                    ast->prefix = d[2] *256+d[3];
                    ast->prefix_count = 0;
                }

                avi->stream_index_2 = n;
                ast->packet_size = size + 8;
                ast->remaining = size;
                goto resync;
            }
        }
        // palette changed chunk
        if (d[0] >= '0' && d[0] <= '9' && d[1] >= '0' && d[1] <= '9'
        && (d[2] == 'p' && d[3] == 'c') && n < s->nb_streams && i + size <= avi->movi_end)
        {
            AVStream *st;
            int first, clr, flags, k, p;

            st = s->streams[n];

            first = get_byte(pb);
            clr = get_byte(pb);
            if (!clr) // all 256 colors used
                clr = 256;
            flags = get_le16(pb);
            p = 4;
            for (k = first; k < clr + first; k++)
            {
                int r, g, b;
                r = get_byte(pb);
                g = get_byte(pb);
                b = get_byte(pb);
                get_byte(pb);
                st->actx->palctrl->palette[k] = b + (g << 8) + (r << 16);
            }
            st->actx->palctrl->palette_changed = 1;
            goto resync;
        }
    }

    return  - 1;
}

static int avi_read_idx1(AVFormatContext *s, int size)
{
    AVIContext *avi = s->priv_data;
    ByteIOContext *pb = &s->pb;
    int nb_index_entries, i;
    AVStream *st;
    AVIStream *ast;
    unsigned int index, tag, flags, pos, len;
    unsigned last_pos =  - 1;

    nb_index_entries = size / 16;
    if (nb_index_entries <= 0)
        return  - 1;

    for (i = 0; i < nb_index_entries; i++)// read the entries and sort them in each stream component
    {
        tag = get_le32(pb);
        flags = get_le32(pb);
        pos = get_le32(pb);
        len = get_le32(pb);

        if (i == 0 && pos > avi->movi_list)
            avi->movi_list = 0;

        pos += avi->movi_list;

        index = ((tag &0xff) - '0') *10; 
        index += ((tag >> 8) &0xff) - '0';
        if (index >= s->nb_streams)
            continue;

        st = s->streams[index];
        ast = st->priv_data;

        if (last_pos == pos)
            avi->non_interleaved = 1;
        else
            av_add_index_entry(st, pos, ast->cum_len, len, 0, (flags &AVIIF_INDEX) ? AVINDEX_KEYFRAME : 0);

        if (ast->sample_size)
            ast->cum_len += len / ast->sample_size;
        else
            ast->cum_len++;
        last_pos = pos;
    }
    return 0;
}

static int guess_ni_flag(AVFormatContext *s)
{
    int i;
    int64_t last_start = 0;
    int64_t first_end = INT64_MAX;

    for (i = 0; i < s->nb_streams; i++)
    {
        AVStream *st = s->streams[i];
        int n = st->nb_index_entries;

        if (n <= 0)
            continue;

        if (st->index_entries[0].pos > last_start)
            last_start = st->index_entries[0].pos;

        if (st->index_entries[n - 1].pos < first_end)
            first_end = st->index_entries[n - 1].pos;
    }
    return last_start > first_end;
}

static int avi_load_index(AVFormatContext *s)
{
    AVIContext *avi = s->priv_data;
    ByteIOContext *pb = &s->pb;
    uint32_t tag, size;
    offset_t pos = url_ftell(pb);

    url_fseek(pb, avi->movi_end, SEEK_SET);

    for (;;)
    {
        if (url_feof(pb))
            break;
        tag = get_le32(pb);
        size = get_le32(pb);

        switch (tag)
        {
        case MKTAG('i', 'd', 'x', '1'):
            if (avi_read_idx1(s, size) < 0)
                goto skip;
            else
                goto the_end;
            break;
        default:
skip: 
	        size += (size &1);
            url_fskip(pb, size);
            break;
        }
    }
the_end: 
    url_fseek(pb, pos, SEEK_SET);
    return 0;
}

static int avi_read_close(AVFormatContext *s)
{
    int i;
    AVIContext *avi = s->priv_data;

    for (i = 0; i < s->nb_streams; i++)
    {
        AVStream *st = s->streams[i];
        AVIStream *ast = st->priv_data;
        av_free(ast);
        av_free(st->actx->extradata);
        av_free(st->actx->palctrl);
    }

    return 0;
}

static int avi_probe(AVProbeData *p)
{
    if (p->buf_size <= 32) // check file header
        return 0;
    if (p->buf[0] == 'R' && p->buf[1] == 'I' && p->buf[2] == 'F' && p->buf[3] == 'F' 
     && p->buf[8] == 'A' && p->buf[9] == 'V' && p->buf[10] == 'I'&& p->buf[11] == ' ')
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}

AVInputFormat avi_iformat =
{
    "avi", 
    sizeof(AVIContext), 
    avi_probe, 
    avi_read_header, 
    avi_read_packet, 
    avi_read_close,
};

int avidec_init(void)
{
    av_register_input_format(&avi_iformat);
    return 0;
}

/*
  AVIF_HASINDEX：标明该AVI文件有"idx1"块   
  AVIF_MUSTUSEINDEX：标明必须根据索引表来指定数据顺序     
  AVIF_ISINTERLEAVED：标明该AVI文件是interleaved格式的   
  AVIF_WASCAPTUREFILE：标明该AVI文件是用捕捉实时视频专门分配的文件   
  AVIF_COPYRIGHTED：标明该AVI文件包含有版权信息   

  AVIF_MUSTUSEINDEX : 表明应用程序需要使用index，而不是物理上的顺序，来定义数据的展现顺序。
                       例如，该标志可以用于创建一个编辑用的帧列表。
// */