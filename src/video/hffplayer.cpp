#include "hffplayer.h"

#include "confile.h"
#include "hlog.h"
#include "hstring.h"
#include "hscope.h"
#include "htime.h"

#include <iostream>
#define DEFAULT_BLOCK_TIMEOUT   10  // s

std::atomic_flag HFFPlayer::s_ffmpeg_init = ATOMIC_FLAG_INIT;

static void list_devices() {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    AVDictionary* options = NULL;
    av_dict_set(&options, "list_devices", "true", 0);
#ifdef _WIN32
    const char drive[] = "dshow";
#elif defined(__linux__)
    const char drive[] = "v4l2";
#else
    const char drive[] = "avfoundation";
#endif
    AVInputFormat* ifmt = av_find_input_format(drive);
    if (ifmt) {
        avformat_open_input(&fmt_ctx, "video=dummy", ifmt, &options);
    }
    avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
    av_dict_free(&options);
}

// NOTE: avformat_open_input,av_read_frame block
static int interrupt_callback(void* opaque) {
    if (opaque == NULL) return 0;
    HFFPlayer* player = (HFFPlayer*)opaque;
    if (player->quit ||
        time(NULL) - player->block_starttime > player->block_timeout) {
        hlogi("interrupt quit=%d media.src=%s", player->quit, player->media.src.c_str());
        return 1;
    }
    return 0;
}

HFFPlayer::HFFPlayer()
: HVideoPlayer()
, HThread() {
    fmt_opts = NULL;
    codec_opts = NULL;
    fmt_ctx = NULL;
    codec_ctx = NULL;
    acodec_ctx = NULL;
    packet = NULL;
    frame = NULL;
    sws_ctx = NULL;
    swr_ctx = NULL;
    block_starttime = time(NULL);
    block_timeout = DEFAULT_BLOCK_TIMEOUT;
    quit = 0;
    video_stream_index=audio_stream_index=-1;
    if (!s_ffmpeg_init.test_and_set()) {
        // av_register_all();
        // avcodec_register_all();
        avformat_network_init();
        avdevice_register_all();
        list_devices();
    }
}

HFFPlayer::~HFFPlayer() {
    close();
}

int HFFPlayer::open() {
    //ifile存的是媒体文件地址，是从media中获取来的，media是打开文件or网络流的时候创建的。
    std::string ifile;

    AVInputFormat* ifmt = NULL;
    switch (media.type) {
    case MEDIA_TYPE_CAPTURE:
    {
        ifile = "video=";
        ifile += media.src;
#ifdef _WIN32
        const char drive[] = "dshow";
#elif defined(__linux__)
        const char drive[] = "v4l2";
#else
        const char drive[] = "avfoundation";
#endif
        ifmt = av_find_input_format(drive);
        if (ifmt == NULL) {
            hloge("Can not find dshow");
            return -5;
        }
    }
        break;
    case MEDIA_TYPE_FILE:
    case MEDIA_TYPE_NETWORK:
        ifile = media.src;
        break;
    default:
        return -10;
    }
    hlogi("ifile:%s", ifile.c_str());
    int ret = 0;
    //创建一个AVFormatContext对象，AVFormatContext是存储音视频封装格式中包含的信息的结构体，是ffmpeg解码的第一步
    fmt_ctx = avformat_alloc_context();
    if (fmt_ctx == NULL) {
        hloge("avformat_alloc_context");
        ret = -10;
        return ret;
    }
    //很秀的defer，当该函数不正常返回的时候清空掉之前创建的AVFormatContext对象，avformat_alloc_context()和avformat_free_context()是成对出现的，这样的好处是处理异常不用写的这么麻烦
    defer (if (ret != 0 && fmt_ctx) {avformat_free_context(fmt_ctx); fmt_ctx = NULL;})
    //如果是网络流媒体
    if (media.type == MEDIA_TYPE_NETWORK) {
        /*
         * 比较前5个字符，第一个参数大就返回>0，一样就返回0，小就返回<0
         * 所以这里的意思应该是看一下是不是rtsp流
         * 然后从config文件里去读，在video大类下面找rtsp_transport,有tcp和udp两种类型，作者默认是tcp，可以修改
         * av_dict_set是给AVFormatContext中添加一对key-value(2,3参数，4是标志位)，这里相当于告诉AVFormatContext对象用tcp还是udp来拉流
         */
        if (strncmp(media.src.c_str(), "rtsp:", 5) == 0) {
            std::string str = g_confile->GetValue("rtsp_transport", "video");
            //std::cout<<str<<"\n";
            if (strcmp(str.c_str(), "tcp") == 0 ||
                strcmp(str.c_str(), "udp") == 0) {
                av_dict_set(&fmt_opts, "rtsp_transport", str.c_str(), 0);
            }
        }
        //设置一个timeout时间，如果超过5s都没响应就别等了
        av_dict_set(&fmt_opts, "stimeout", "5000000", 0);   // us
    }
    //设置一个buff上限，这里选择了2048000字节
    av_dict_set(&fmt_opts, "buffer_size", "2048000", 0);
    //经典回调函数，为了配合前面设置的timeout函数
    fmt_ctx->interrupt_callback.callback = interrupt_callback;
    fmt_ctx->interrupt_callback.opaque = this;
    //要开始读了，记录一下时间点，从这个时间点朝后计算所需时间，如果时间到了后面的打开失败就进入defer处理了
    block_starttime = time(NULL);
    /*
     * avformat_open_input函数详解
       该函数用于打开一个输入的封装器。在调用该函数之前，须确保av_register_all()和avformat_network_init()已调用。
       参数说明：
       AVFormatContext **ps, 格式化的上下文。要注意，如果传入的是一个AVFormatContext*的指针，则该空间须自己手动清理，若传入的指针为空，则FFmpeg会内部自己创建。
       const char *url, 传入的地址。支持http,RTSP,以及普通的本地文件。地址最终会存入到AVFormatContext结构体当中。
       AVInputFormat *fmt, 指定输入的封装格式。一般传NULL，由FFmpeg自行探测。
       AVDictionary **options, 其它参数设置。它是一个字典，用于参数传递，不传则写NULL。参见：libavformat/options_table.h,其中包含了它支持的参数设置。
     */
    ret = avformat_open_input(&fmt_ctx, ifile.c_str(), ifmt, &fmt_opts);
    if (ret != 0) {
        hloge("Open input file[%s] failed: %d", ifile.c_str(), ret);
        return ret;
    }
    //都成功读到了，不需要在测算是不是超时了，把回调函数置空
    fmt_ctx->interrupt_callback.callback = NULL;
    //如果这次打开失败了，一样的，把AVFormatContext给处理掉
    defer (if (ret != 0 && fmt_ctx) {avformat_close_input(&fmt_ctx);})
    //avformat_find_stream_info()函数是用于获取媒体文件中每个音视频流的详细信息的函数，包括解码器类型、采样率、声道数、码率、关键帧等信息。
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret != 0) {
        hloge("Can not find stream: %d", ret);
        return ret;
    }
    hlogi("stream_num=%d", fmt_ctx->nb_streams);
    //查找视频流，音频流，字幕流下标
    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    subtitle_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    hlogi("video_stream_index=%d", video_stream_index);
    hlogi("audio_stream_index=%d", audio_stream_index);
    hlogi("subtitle_stream_index=%d", subtitle_stream_index);
    //如果没视频流，那也没播的必要了，直接报错返回
    if (video_stream_index < 0) {
        hloge("Can not find video stream.");
        ret = -20;
        return ret;
    }
    //选择一种音视频同步方式，0代表无音频，1代表视频同步音频，2代表都向系统时钟同步
    if(g_confile->GetValue("ctrl", "media").c_str())
        ctrl=g_confile->GetValue("ctrl", "media").c_str()[0]-'0';
    else
        ctrl=0;
    if(audio_stream_index<0)
        ctrl=0;
    //获取视频流
    AVStream* video_stream = fmt_ctx->streams[video_stream_index];
    //获取时间刻度，分别代表分子分母，视频一般为1/25
    video_time_base_num = video_stream->time_base.num;
    video_time_base_den = video_stream->time_base.den;
    hlogi("video_stream time_base=%d/%d", video_stream->time_base.num, video_stream->time_base.den);
    //保存视频流的基本参数信息，取代了之前的AVCodecContext
    /*codectype：编解码器类型，取值为AVMEDIATYPEVIDEO、AVMEDIATYPEAUDIO、AVMEDIATYPE_SUBTITLE之一。
     codec_id：编解码器的ID，取值为枚举类型AVCodecID中的一种。
     codec_tag：编解码器标签。
     extradata：对于某些编码器（如H.264）需要额外的信息来解码，这些信息称为“extradata”。该成员变量保存了这些额外信息的地址。
     extradata_size：extradata所占用的字节数。
     format：音视频帧的像素或采样格式，例如对于视频来说，可以是AVPIXFMTYUV420P、AVPIXFMTNV12等等。对于音频来说，可以是AVSAMPLEFMTS16、AVSAMPLEFMTFLTP等等。
     bit_rate：比特率，用于描述音视频流的质量，单位为bps。
     bitspercoded_sample：每个编码的样本所占用的位数。
     bitsperraw_sample：每个采样的样本所占用的位数。
     profile：编解码器的协议等级。
     level：编解码器的协议等级。
     width：视频帧宽度。
     height：视频帧高度。
     sample_rate：音频采样率。
     channels：音频声道数。
     samplefmt：音频采样格式，例如AVSAMPLEFMTS16、AVSAMPLEFMT_FLTP等等。
     channellayout：音频声道布局，例如AVCHLAYOUTMONO、AVCHLAYOUT_STEREO等等。
     sampleaspectratio：音视频帧宽高比。
     fpsfirstnum：帧率分子。
     fpssecondnum：帧率分母。
     delay：编码器的编码时延。
     seek_preroll：如果需要进行关键帧的跳转，这个字段表示需要跳过的帧数。
*/
    AVCodecParameters* codec_param = video_stream->codecpar;
    hlogi("codec_id=%d:%s", codec_param->codec_id, avcodec_get_name(codec_param->codec_id));
    //视频解码器，包含了编解码器的类型、名称、支持的音视频格式、编解码函数等。通过AVCodec结构体，可以查询和获取系统中可用的编解码器，并与AVCodecParameters关联以进行音视频编解码操作。
    AVCodec* codec = NULL;
    //进入Switch判断，具体采用何种方式解码，找到了之后更新一下real_decode_mode
    if (decode_mode != SOFTWARE_DECODE) {
try_hardware_decode:
        //返回一个codec_id对应的编码格式，我自己的视频返回的是h264
        std::string decoder(avcodec_get_name(codec_param->codec_id));
        //std::cout<<decoder<<"\n";
        if (decode_mode == HARDWARE_DECODE_CUVID) {
            decoder += "_cuvid";
            real_decode_mode = HARDWARE_DECODE_CUVID;
        }
        else if (decode_mode == HARDWARE_DECODE_QSV) {
            decoder += "_qsv";
            real_decode_mode = HARDWARE_DECODE_QSV;
        }
        //根据编码格式和解码手段组合，找到对应的AVCodec，比如我这里就是h264_cuvid
        codec = avcodec_find_decoder_by_name(decoder.c_str());
        if (codec == NULL) {
            hlogi("Can not find decoder %s", decoder.c_str());
            // goto try_software_decode;
        }
        hlogi("decoder=%s", decoder.c_str());
    }
    //硬解码没找到，尝试软解
    if (codec == NULL) {
try_software_decode:
        //软解就比较直接了，直接去找一个codec出来就行了。
        codec = avcodec_find_decoder(codec_param->codec_id);
        if (codec == NULL) {
            hloge("Can not find decoder %s", avcodec_get_name(codec_param->codec_id));
            ret = -30;
            return ret;
        }
        real_decode_mode = SOFTWARE_DECODE;
    }

    hlogi("codec_name: %s=>%s", codec->name, codec->long_name);
    //获取编解码器上下文，没明白为啥要获取一个AVCodecContext出来，不是有AVCodecParameters了吗
    /*
    AVCodecParameters和AVCodecContext的区别
    AVCodecContext结构体是一个重量级的结构体，包含了AVCodecParameters结构体所有信息，同时还包含了一些AVCodecParameters中没有的参数，比如一些与编解码器相关的控制信息；
    AVCodecContext结构体通常用于描述音视频的编解码器，可用于解码和编码等操作；
    当进行解码时，通常先从AVCodecParameters结构体中创建一个AVCodecContext结构体，再使用该结构体进行解码。
    在使用AVCodecParameters和AVCodecContext时，我们需要明确它们之间的关系。AVCodecContext结构体是AVCodecParameters结构体的一个超集.
    AVCodecParameters结构体中包含了AVCodecContext结构体中的大部分参数，但是AVCodecContext结构体中还包含了很多AVCodecParameters没有的编解码器相关参数。
    所以，在一些与编解码器有关的操作中，我们需要使用AVCodecContext，而在仅仅获取音视频流的参数信息时，我们可以使用AVCodecParameters。
    */
    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == NULL) {
        hloge("avcodec_alloc_context3");
        ret = -40;
        return ret;
    }
    defer (if (ret != 0 && codec_ctx) {avcodec_free_context(&codec_ctx); codec_ctx = NULL;})
    //将AVCodecParameters结构体中码流参数拷贝到AVCodecContext结构体中
    ret = avcodec_parameters_to_context(codec_ctx, codec_param);
    if (ret != 0) {
        hloge("avcodec_parameters_to_context error: %d", ret);
        return ret;
    }
    //不明白这是啥意思？？？
    //(1)当AVCodecContext.refcounted_rames被设置为1，该AVFrame被引用计数，返回的引用属于调用者。
    //当不再需要AVFrame时，调用者必须使用av_frame_unref()来释放frame。只有在av_frame_is_writable()返回1，调用者才可以向frame中写入数据。
    //(2)当AVCodecContextrefcounted_frames 被设置为0，返回的引用属于解码器，只有在下一次调用该函数或关闭或刷新解码器之前有效。调用者不能向AVFrame 中写入数据。
    if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
    }
    //打开编解码器
    ret = avcodec_open2(codec_ctx, codec, &codec_opts);
    //如果打开失败了，重新来过吧，试试软解行不行
    if (ret != 0) {
        if (real_decode_mode != SOFTWARE_DECODE) {
            hlogi("Can not open hardwrae codec error: %d, try software codec.", ret);
            goto try_software_decode;
        }
        hloge("Can not open software codec error: %d", ret);
        return ret;
    }
    // 选择丢弃哪些packets,默认只丢size为0的
    video_stream->discard = AVDISCARD_DEFAULT;
    //一些参数赋值，宽高，像素格式，dw和dh是对齐后的宽高，因为编码格式，宏块内都是16*16的，不对齐可能会导致绿边。
    int sw, sh, dw, dh;
    sw = codec_ctx->width;
    sh = codec_ctx->height;
    src_pix_fmt = codec_ctx->pix_fmt;
    hlogi("sw=%d sh=%d src_pix_fmt=%d:%s", sw, sh, src_pix_fmt, av_get_pix_fmt_name(src_pix_fmt));
    if (sw <= 0 || sh <= 0 || src_pix_fmt == AV_PIX_FMT_NONE) {
        hloge("Codec parameters wrong!");
        ret = -45;
        return ret;
    }

    dw = sw >> 2 << 2; // align = 4
    dh = sh;
    //目标像素格式
    dst_pix_fmt = AV_PIX_FMT_YUV420P;
    //根据配置文件来确定具体的目标像素格式
    std::string str = g_confile->GetValue("dst_pix_fmt", "video");
    if (!str.empty()) {
        if (strcmp(str.c_str(), "YUV") == 0) {
            dst_pix_fmt = AV_PIX_FMT_YUV420P;
        }
        else if (strcmp(str.c_str(), "RGB") == 0) {
            dst_pix_fmt = AV_PIX_FMT_BGR24;
        }
    }
    hlogi("dw=%d dh=%d dst_pix_fmt=%d:%s", dw, dh, dst_pix_fmt, av_get_pix_fmt_name(dst_pix_fmt));
    //初始化转换函数
    sws_ctx = sws_getContext(sw, sh, src_pix_fmt, dw, dh, dst_pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
    if (sws_ctx == NULL) {
        hloge("sws_getContext");
        ret = -50;
        return ret;
    }
    //中间packet和frame，不用管
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    //赋值
    hframe.w = dw;
    hframe.h = dh;
    // ARGB,不确定什么格式，先开辟个大的空间保证肯定放得下
    hframe.buf.resize(dw * dh * 4);
    //根据具体的像素格式进行赋值，data和linesize的用法值得品味
    //至于为啥在这里就可以定下来，因为avpacket是压缩后的，avframe没压缩，确定宽高格式之后就知道空间大小了
    if (dst_pix_fmt == AV_PIX_FMT_YUV420P) {
        hframe.type = PIX_FMT_IYUV;
        hframe.bpp = 12;
        int y_size = dw * dh;
        hframe.buf.len = y_size * 3 / 2;
        data[0] = (uint8_t*)hframe.buf.base;
        data[1] = data[0] + y_size;
        data[2] = data[1] + y_size / 4;
        linesize[0] = dw;
        linesize[1] = linesize[2] = dw / 2;
    }
    else {
        dst_pix_fmt = AV_PIX_FMT_BGR24;
        hframe.type = PIX_FMT_BGR;
        hframe.bpp = 24;
        hframe.buf.len = dw * dh * 3;
        data[0] = (uint8_t*)hframe.buf.base;
        linesize[0] = dw * 3;
    }

    // HVideoPlayer member vars
    //很好理解，算一下fps是多少而已
    if (video_stream->avg_frame_rate.num && video_stream->avg_frame_rate.den) {
        fps = video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    }
    //把原始的宽高(没对齐的)的存了一下，不知道是想干嘛，反正就是初始化了一些参数
    width = sw;
    height = sh;
    duration = 0;
    start_time = 0;
    eof = 0;
    error = 0;
    //求一下视频总时长
    if (video_time_base_num && video_time_base_den) {
        if (video_stream->duration > 0) {
            duration = video_stream->duration / (double)video_time_base_den * video_time_base_num * 1000;
        }
        if (video_stream->start_time > 0) {
            start_time = video_stream->start_time / (double)video_time_base_den * video_time_base_num * 1000;
        }
    }
    hlogi("fps=%d duration=%lldms start_time=%lldms", fps, duration, start_time);
    //如果有音频并且你要播，那就按照差不多的操作把音频解码也初始化一下,有问题直接赋给ret，一并处理
    if(ctrl)
        if(ret==0)
            ret=open_audio();
        else
            open_audio();
    //准备工作干完了，设置一下线程解码一次要睡多久
    //HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000 / fps);
    HThread::setSleepPolicy(HThread::SLEEP_FOR,0);
    return ret;
}
int HFFPlayer::open_audio(){
    int ret=0;
    //获取音频流
    AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
    //保存音频流的基本参数信息，取代了之前的AVCodecContext
    /*codectype：编解码器类型，取值为AVMEDIATYPEVIDEO、AVMEDIATYPEAUDIO、AVMEDIATYPE_SUBTITLE之一。
     codec_id：编解码器的ID，取值为枚举类型AVCodecID中的一种。
     codec_tag：编解码器标签。
     extradata：对于某些编码器（如H.264）需要额外的信息来解码，这些信息称为“extradata”。该成员变量保存了这些额外信息的地址。
     extradata_size：extradata所占用的字节数。
     format：音视频帧的像素或采样格式，例如对于视频来说，可以是AVPIXFMTYUV420P、AVPIXFMTNV12等等。对于音频来说，可以是AVSAMPLEFMTS16、AVSAMPLEFMTFLTP等等。
     bit_rate：比特率，用于描述音视频流的质量，单位为bps。
     bitspercoded_sample：每个编码的样本所占用的位数。
     bitsperraw_sample：每个采样的样本所占用的位数。
     profile：编解码器的协议等级。
     level：编解码器的协议等级。
     width：视频帧宽度。
     height：视频帧高度。
     sample_rate：音频采样率。
     channels：音频声道数。
     samplefmt：音频采样格式，例如AVSAMPLEFMTS16、AVSAMPLEFMT_FLTP等等。
     channellayout：音频声道布局，例如AVCHLAYOUTMONO、AVCHLAYOUT_STEREO等等。
     sampleaspectratio：音视频帧宽高比。
     fpsfirstnum：帧率分子。
     fpssecondnum：帧率分母。
     delay：编码器的编码时延。
     seek_preroll：如果需要进行关键帧的跳转，这个字段表示需要跳过的帧数。
*/
    AVCodecParameters* acodec_param = audio_stream->codecpar;
    audio_time_base_num=audio_stream->time_base.num;
    audio_time_base_den=audio_stream->time_base.den;
    hlogi("codec_id=%d:%s", acodec_param->codec_id, avcodec_get_name(acodec_param->codec_id));
    //音频解码器，包含了编解码器的类型、名称、支持的音视频格式、编解码函数等。通过AVCodec结构体，可以查询和获取系统中可用的编解码器，并与AVCodecParameters关联以进行音视频编解码操作。
    AVCodec* codec = NULL;
    codec = avcodec_find_decoder(acodec_param->codec_id);
    hlogi("codec_name: %s=>%s", codec->name, codec->long_name);
    //获取编解码器上下文，没明白为啥要获取一个AVCodecContext出来，不是有AVCodecParameters了吗
    /*
    AVCodecParameters和AVCodecContext的区别
    AVCodecContext结构体是一个重量级的结构体，包含了AVCodecParameters结构体所有信息，同时还包含了一些AVCodecParameters中没有的参数，比如一些与编解码器相关的控制信息；
    AVCodecContext结构体通常用于描述音视频的编解码器，可用于解码和编码等操作；
    当进行解码时，通常先从AVCodecParameters结构体中创建一个AVCodecContext结构体，再使用该结构体进行解码。
    在使用AVCodecParameters和AVCodecContext时，我们需要明确它们之间的关系。AVCodecContext结构体是AVCodecParameters结构体的一个超集.
    AVCodecParameters结构体中包含了AVCodecContext结构体中的大部分参数，但是AVCodecContext结构体中还包含了很多AVCodecParameters没有的编解码器相关参数。
    所以，在一些与编解码器有关的操作中，我们需要使用AVCodecContext，而在仅仅获取音视频流的参数信息时，我们可以使用AVCodecParameters。
    */
    acodec_ctx = avcodec_alloc_context3(codec);
    if (acodec_ctx == NULL) {
        hloge("avcodec_alloc_context3-audio");
        ret = -40-100;
        ctrl=0;
        return ret;
    }
    defer (if (ret != 0 && acodec_ctx) {avcodec_free_context(&acodec_ctx); acodec_ctx = NULL;})
    //将AVCodecParameters结构体中码流参数拷贝到AVCodecContext结构体中
    ret = avcodec_parameters_to_context(acodec_ctx, acodec_param);
    if (ret != 0) {
        hloge("avcodec_parameters_to_context error-audio: %d", ret);
        ctrl=0;
        return ret;
    }
    //不明白这是啥意思？？？
    //(1)当AVCodecContext.refcounted_rames被设置为1，该AVFrame被引用计数，返回的引用属于调用者。
    //当不再需要AVFrame时，调用者必须使用av_frame_unref()来释放frame。只有在av_frame_is_writable()返回1，调用者才可以向frame中写入数据。
    //(2)当AVCodecContextrefcounted_frames 被设置为0，返回的引用属于解码器，只有在下一次调用该函数或关闭或刷新解码器之前有效。调用者不能向AVFrame 中写入数据。
    if (acodec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || acodec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
    }
    //打开编解码器
    ret = avcodec_open2(acodec_ctx, codec, &codec_opts);
    //如果打开失败了，返回
    if (ret != 0) {
        hloge("Can not open software codec error-audio: %d", ret);
        ctrl=0;
        return ret;
    }
    // 选择丢弃哪些packets,默认只丢size为0的
    audio_stream->discard = AVDISCARD_DEFAULT;
    //一些参数
    audio_src_smft = acodec_ctx->sample_fmt;
    //目标格式
    audio_dst_smft = AV_SAMPLE_FMT_S16;
    //根据配置文件来确定具体的目标像素格式
    std::string str = g_confile->GetValue("dst_smft", "audio");
    if (!str.empty()) {
        if (strcmp(str.c_str(), "AV_SAMPLE_FMT_S16") == 0) {
            audio_dst_smft = AV_SAMPLE_FMT_S16;
        }
        else
            audio_dst_smft = AV_SAMPLE_FMT_S32;
    }
    hlogi("audio_dst_smft=%d", audio_dst_smft);
    //初始化转换函数
    swr_ctx = swr_alloc_set_opts(NULL,acodec_ctx->channel_layout,
            audio_dst_smft,
            acodec_ctx->sample_rate,
            acodec_ctx->channel_layout,
            acodec_ctx->sample_fmt,
            acodec_ctx->sample_rate,
            0,
            NULL);
    if (swr_ctx == NULL) {
        hloge("swr_getContext");
        ret = -50-100;
        ctrl=0;
        return ret;
    }
    swr_init(swr_ctx);
    //赋值
    aframe.chanels = acodec_ctx->channels;
    aframe.sample_rate = acodec_ctx->sample_rate;
    // 不确定什么格式，先开辟个大的空间保证肯定放得下
    aframe.buf.resize(acodec_ctx->frame_size * acodec_ctx->channels * 32);
    //根据具体的格式进行赋值，data和linesize的用法值得品味
    //至于为啥在这里就可以定下来，因为avpacket是压缩后的，avframe没压缩，确定格式之后就知道空间大小了
    if (audio_dst_smft == AV_SAMPLE_FMT_S16) {
        aframe.type = AV_SAMPLE_FMT_S16;
        int y_size = acodec_ctx->frame_size * acodec_ctx->channels *16;
        aframe.buf.len = y_size;
        aframe.bpp=16;
        audio_data[0] = (uint8_t*)aframe.buf.base;
        aframe.duration=acodec_ctx->frame_size*1000/acodec_ctx->sample_rate;
        audio_linesize[0] = y_size/8;
    }
    else {
        audio_dst_smft = AV_SAMPLE_FMT_S32;
        aframe.type = AV_SAMPLE_FMT_S32;
        int y_size = acodec_ctx->frame_size * acodec_ctx->channels *32;
        aframe.buf.len = y_size;
        aframe.bpp=32;
        aframe.duration=acodec_ctx->frame_size*1000/acodec_ctx->sample_rate;
        audio_data[0] = (uint8_t*)hframe.buf.base;
        audio_linesize[0] = y_size/8;
    }
    return ret;
}
int HFFPlayer::close() {
    if (fmt_opts) {
        av_dict_free(&fmt_opts);
        fmt_opts = NULL;
    }

    if (codec_opts) {
        av_dict_free(&codec_opts);
        codec_opts = NULL;
    }

    if (codec_ctx) {
        avcodec_close(codec_ctx);
        avcodec_free_context(&codec_ctx);
        codec_ctx = NULL;
    }
    if (acodec_ctx) {
        avcodec_close(acodec_ctx);
        avcodec_free_context(&acodec_ctx);
        acodec_ctx = NULL;
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
        avformat_free_context(fmt_ctx);
        fmt_ctx = NULL;
    }

    if (frame) {
        av_frame_unref(frame);
        av_frame_free(&frame);
        frame = NULL;
    }

    if (packet) {
        av_packet_unref(packet);
        av_packet_free(&packet);
        packet = NULL;
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }

    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = NULL;
    }

    hframe.buf.cleanup();
    aframe.buf.cleanup();
    return 0;
}

int HFFPlayer::seek(int64_t ms) {
    if (fmt_ctx) {
        clear_frame_cache();
        clear_aframe_cache();
        hlogi("seek=>%lldms", ms);
//        if(audio_stream_index>=0)
//            return av_seek_frame(fmt_ctx, audio_stream_index,
//                                 (start_time+ms)/1000/(double)audio_time_base_num*audio_time_base_den,
//                                 AVSEEK_FLAG_BACKWARD);
        return av_seek_frame(fmt_ctx, video_stream_index,
                (start_time+ms)/1000/(double)video_time_base_num*video_time_base_den,
                AVSEEK_FLAG_BACKWARD);
    }
    return 0;
}

bool HFFPlayer::doPrepare() {
    int ret = open();
    if (ret != 0) {
        if (!quit) {
            error = ret;
            event_callback(HPLAYER_OPEN_FAILED);
        }
        return false;
    }
    else {
        event_callback(HPLAYER_OPENED);
    }
    return true;
}

bool HFFPlayer::doFinish() {
    int ret = close();
    event_callback(HPLAYER_CLOSED);
    return ret == 0;
}

void HFFPlayer::doTask() {
    if(frame_buf.frame_stats.size>frame_buf.cache_num-10||aframe_buf.frame_stats.size>aframe_buf.cache_num-10)
    {
        //std::cout<<"frame_buf: "<<frame_buf.frame_stats.size<<" "<<aframe_buf.frame_stats.size<<" "<<frame_buf.cache_num<<" "<<aframe_buf.cache_num<<"\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000/fps));
        return ;
    }
    // loop until get a video frame
    AVCodecContext*  ctx;
    int type=-1;
    while (!quit) {
        av_init_packet(packet);

        fmt_ctx->interrupt_callback.callback = interrupt_callback;
        fmt_ctx->interrupt_callback.opaque = this;
        block_starttime = time(NULL);
        //hlogi("av_read_frame");
        int ret = av_read_frame(fmt_ctx, packet);
        //hlogi("av_read_frame retval=%d", ret);
        fmt_ctx->interrupt_callback.callback = NULL;
        if (ret != 0) {
            hlogi("No frame: %d", ret);
            if (!quit) {
                if (ret == AVERROR_EOF || avio_feof(fmt_ctx->pb)) {
                    eof = 1;
                    event_callback(HPLAYER_EOF);
                }
                else {
                    error = ret;
                    event_callback(HPLAYER_ERROR);
                }
            }
            return;
        }

        // NOTE: if not call av_packet_unref, memory leak.
        defer (av_packet_unref(packet);)
        type=packet->stream_index;
        // hlogi("stream_index=%d data=%p len=%d", packet->stream_index, packet->data, packet->size);
        if (packet->stream_index == video_stream_index)
            ctx=codec_ctx;
        else if(ctrl&&packet->stream_index == audio_stream_index)
        {
            ctx=acodec_ctx;
        }
        else
            continue;
#if 1
        // hlogi("avcodec_send_packet");
        ret = avcodec_send_packet(ctx, packet);
        if (ret != 0) {
            hloge("avcodec_send_packet error: %d", ret);
            return;
        }
        // hlogi("avcodec_receive_frame");
        ret = avcodec_receive_frame(ctx, frame);
        if (ret != 0) {
            if (ret != -EAGAIN) {
                hloge("avcodec_receive_frame error: %d", ret);
                return;
            }
        }
        else {
            break;
        }
#else
        int got_pic = 0;
        // hlogi("avcodec_decode_video2");
        ret = avcodec_decode_video2(codec_ctx, frame, &got_pic, packet);
        // hlogi("avcodec_decode_video2 retval=%d got_pic=%d", ret, got_pic);
        if (ret < 0) {
            hloge("decoder error: %d", ret);
            return;
        }

        if (got_pic)    break;  // exit loop
#endif
    }
    if(type<0)
        return ;
    if (sws_ctx&&type == video_stream_index) {
        // hlogi("sws_scale w=%d h=%d data=%p", frame->width, frame->height, frame->data);
        int h = sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, data, linesize);
        // hlogi("sws_scale h=%d", h);
        if (h <= 0 || h != frame->height) {
            return;
        }
    }
    if(swr_ctx&&type == audio_stream_index&&ctrl)
    {
        int ret=swr_convert(swr_ctx,audio_data,audio_linesize[0],(const uint8_t**)frame->data,frame->nb_samples);
    }
//    if(type==video_stream_index)
//        std::cout<<"video: "<<frame->pts / (double)video_time_base_den * video_time_base_num * 1000<<"\n";
//    else
//        std::cout<<"audio: "<<frame->pts / (double)audio_time_base_den * audio_time_base_num * 1000<<"\n";
    if(type != video_stream_index)
    {
        //std::cout<<"acode\n";
        if(type==audio_stream_index)
        {
            if (audio_time_base_num && audio_time_base_den)
                aframe.ts=frame->pts / (double)audio_time_base_den * audio_time_base_num * 1000;
            push_frame(&aframe);
            //std::cout<<aframe.duration<<" "<<;
        }
        doTask();
    }
    if (video_time_base_num && video_time_base_den) {
        hframe.ts = frame->pts / (double)video_time_base_den * video_time_base_num * 1000;
    }
    // hlogi("ts=%lldms", hframe.ts);
    if(type==video_stream_index)
    push_frame(&hframe);
    if(get_frame_stats().size>frame_buf.cache_num/2||get_aframe_stats().size>aframe_buf.cache_num/2)
    {
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 2*1000 / fps);
    }
    else if(get_frame_stats().size>frame_buf.cache_num/4||get_aframe_stats().size>aframe_buf.cache_num/4)
    {
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000 / fps);
    }
    else
    {
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 0);
    }
}
