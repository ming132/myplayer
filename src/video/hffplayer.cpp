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
    //ifile�����ý���ļ���ַ���Ǵ�media�л�ȡ���ģ�media�Ǵ��ļ�or��������ʱ�򴴽��ġ�
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
    //����һ��AVFormatContext����AVFormatContext�Ǵ洢����Ƶ��װ��ʽ�а�������Ϣ�Ľṹ�壬��ffmpeg����ĵ�һ��
    fmt_ctx = avformat_alloc_context();
    if (fmt_ctx == NULL) {
        hloge("avformat_alloc_context");
        ret = -10;
        return ret;
    }
    //�����defer�����ú������������ص�ʱ����յ�֮ǰ������AVFormatContext����avformat_alloc_context()��avformat_free_context()�ǳɶԳ��ֵģ������ĺô��Ǵ����쳣����д����ô�鷳
    defer (if (ret != 0 && fmt_ctx) {avformat_free_context(fmt_ctx); fmt_ctx = NULL;})
    //�����������ý��
    if (media.type == MEDIA_TYPE_NETWORK) {
        /*
         * �Ƚ�ǰ5���ַ�����һ��������ͷ���>0��һ���ͷ���0��С�ͷ���<0
         * �����������˼Ӧ���ǿ�һ���ǲ���rtsp��
         * Ȼ���config�ļ���ȥ������video����������rtsp_transport,��tcp��udp�������ͣ�����Ĭ����tcp�������޸�
         * av_dict_set�Ǹ�AVFormatContext�����һ��key-value(2,3������4�Ǳ�־λ)�������൱�ڸ���AVFormatContext������tcp����udp������
         */
        if (strncmp(media.src.c_str(), "rtsp:", 5) == 0) {
            std::string str = g_confile->GetValue("rtsp_transport", "video");
            //std::cout<<str<<"\n";
            if (strcmp(str.c_str(), "tcp") == 0 ||
                strcmp(str.c_str(), "udp") == 0) {
                av_dict_set(&fmt_opts, "rtsp_transport", str.c_str(), 0);
            }
        }
        //����һ��timeoutʱ�䣬�������5s��û��Ӧ�ͱ����
        av_dict_set(&fmt_opts, "stimeout", "5000000", 0);   // us
    }
    //����һ��buff���ޣ�����ѡ����2048000�ֽ�
    av_dict_set(&fmt_opts, "buffer_size", "2048000", 0);
    //����ص�������Ϊ�����ǰ�����õ�timeout����
    fmt_ctx->interrupt_callback.callback = interrupt_callback;
    fmt_ctx->interrupt_callback.opaque = this;
    //Ҫ��ʼ���ˣ���¼һ��ʱ��㣬�����ʱ��㳯���������ʱ�䣬���ʱ�䵽�˺���Ĵ�ʧ�ܾͽ���defer������
    block_starttime = time(NULL);
    /*
     * avformat_open_input�������
       �ú������ڴ�һ������ķ�װ�����ڵ��øú���֮ǰ����ȷ��av_register_all()��avformat_network_init()�ѵ��á�
       ����˵����
       AVFormatContext **ps, ��ʽ���������ġ�Ҫע�⣬����������һ��AVFormatContext*��ָ�룬��ÿռ����Լ��ֶ������������ָ��Ϊ�գ���FFmpeg���ڲ��Լ�������
       const char *url, ����ĵ�ַ��֧��http,RTSP,�Լ���ͨ�ı����ļ�����ַ���ջ���뵽AVFormatContext�ṹ�嵱�С�
       AVInputFormat *fmt, ָ������ķ�װ��ʽ��һ�㴫NULL����FFmpeg����̽�⡣
       AVDictionary **options, �����������á�����һ���ֵ䣬���ڲ������ݣ�������дNULL���μ���libavformat/options_table.h,���а�������֧�ֵĲ������á�
     */
    ret = avformat_open_input(&fmt_ctx, ifile.c_str(), ifmt, &fmt_opts);
    if (ret != 0) {
        hloge("Open input file[%s] failed: %d", ifile.c_str(), ret);
        return ret;
    }
    //���ɹ������ˣ�����Ҫ�ڲ����ǲ��ǳ�ʱ�ˣ��ѻص������ÿ�
    fmt_ctx->interrupt_callback.callback = NULL;
    //�����δ�ʧ���ˣ�һ���ģ���AVFormatContext�������
    defer (if (ret != 0 && fmt_ctx) {avformat_close_input(&fmt_ctx);})
    //avformat_find_stream_info()���������ڻ�ȡý���ļ���ÿ������Ƶ������ϸ��Ϣ�ĺ������������������͡������ʡ������������ʡ��ؼ�֡����Ϣ��
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret != 0) {
        hloge("Can not find stream: %d", ret);
        return ret;
    }
    hlogi("stream_num=%d", fmt_ctx->nb_streams);
    //������Ƶ������Ƶ������Ļ���±�
    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    subtitle_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    hlogi("video_stream_index=%d", video_stream_index);
    hlogi("audio_stream_index=%d", audio_stream_index);
    hlogi("subtitle_stream_index=%d", subtitle_stream_index);
    //���û��Ƶ������Ҳû���ı�Ҫ�ˣ�ֱ�ӱ�����
    if (video_stream_index < 0) {
        hloge("Can not find video stream.");
        ret = -20;
        return ret;
    }
    //ѡ��һ������Ƶͬ����ʽ��0��������Ƶ��1������Ƶͬ����Ƶ��2������ϵͳʱ��ͬ��
    if(g_confile->GetValue("ctrl", "media").c_str())
        ctrl=g_confile->GetValue("ctrl", "media").c_str()[0]-'0';
    else
        ctrl=0;
    if(audio_stream_index<0)
        ctrl=0;
    //��ȡ��Ƶ��
    AVStream* video_stream = fmt_ctx->streams[video_stream_index];
    //��ȡʱ��̶ȣ��ֱ������ӷ�ĸ����Ƶһ��Ϊ1/25
    video_time_base_num = video_stream->time_base.num;
    video_time_base_den = video_stream->time_base.den;
    hlogi("video_stream time_base=%d/%d", video_stream->time_base.num, video_stream->time_base.den);
    //������Ƶ���Ļ���������Ϣ��ȡ����֮ǰ��AVCodecContext
    /*codectype������������ͣ�ȡֵΪAVMEDIATYPEVIDEO��AVMEDIATYPEAUDIO��AVMEDIATYPE_SUBTITLE֮һ��
     codec_id�����������ID��ȡֵΪö������AVCodecID�е�һ�֡�
     codec_tag�����������ǩ��
     extradata������ĳЩ����������H.264����Ҫ�������Ϣ�����룬��Щ��Ϣ��Ϊ��extradata�����ó�Ա������������Щ������Ϣ�ĵ�ַ��
     extradata_size��extradata��ռ�õ��ֽ�����
     format������Ƶ֡�����ػ������ʽ�����������Ƶ��˵��������AVPIXFMTYUV420P��AVPIXFMTNV12�ȵȡ�������Ƶ��˵��������AVSAMPLEFMTS16��AVSAMPLEFMTFLTP�ȵȡ�
     bit_rate�������ʣ�������������Ƶ������������λΪbps��
     bitspercoded_sample��ÿ�������������ռ�õ�λ����
     bitsperraw_sample��ÿ��������������ռ�õ�λ����
     profile�����������Э��ȼ���
     level�����������Э��ȼ���
     width����Ƶ֡��ȡ�
     height����Ƶ֡�߶ȡ�
     sample_rate����Ƶ�����ʡ�
     channels����Ƶ��������
     samplefmt����Ƶ������ʽ������AVSAMPLEFMTS16��AVSAMPLEFMT_FLTP�ȵȡ�
     channellayout����Ƶ�������֣�����AVCHLAYOUTMONO��AVCHLAYOUT_STEREO�ȵȡ�
     sampleaspectratio������Ƶ֡��߱ȡ�
     fpsfirstnum��֡�ʷ��ӡ�
     fpssecondnum��֡�ʷ�ĸ��
     delay���������ı���ʱ�ӡ�
     seek_preroll�������Ҫ���йؼ�֡����ת������ֶα�ʾ��Ҫ������֡����
*/
    AVCodecParameters* codec_param = video_stream->codecpar;
    hlogi("codec_id=%d:%s", codec_param->codec_id, avcodec_get_name(codec_param->codec_id));
    //��Ƶ�������������˱�����������͡����ơ�֧�ֵ�����Ƶ��ʽ������뺯���ȡ�ͨ��AVCodec�ṹ�壬���Բ�ѯ�ͻ�ȡϵͳ�п��õı������������AVCodecParameters�����Խ�������Ƶ����������
    AVCodec* codec = NULL;
    //����Switch�жϣ�������ú��ַ�ʽ���룬�ҵ���֮�����һ��real_decode_mode
    if (decode_mode != SOFTWARE_DECODE) {
try_hardware_decode:
        //����һ��codec_id��Ӧ�ı����ʽ�����Լ�����Ƶ���ص���h264
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
        //���ݱ����ʽ�ͽ����ֶ���ϣ��ҵ���Ӧ��AVCodec���������������h264_cuvid
        codec = avcodec_find_decoder_by_name(decoder.c_str());
        if (codec == NULL) {
            hlogi("Can not find decoder %s", decoder.c_str());
            // goto try_software_decode;
        }
        hlogi("decoder=%s", decoder.c_str());
    }
    //Ӳ����û�ҵ����������
    if (codec == NULL) {
try_software_decode:
        //���ͱȽ�ֱ���ˣ�ֱ��ȥ��һ��codec���������ˡ�
        codec = avcodec_find_decoder(codec_param->codec_id);
        if (codec == NULL) {
            hloge("Can not find decoder %s", avcodec_get_name(codec_param->codec_id));
            ret = -30;
            return ret;
        }
        real_decode_mode = SOFTWARE_DECODE;
    }

    hlogi("codec_name: %s=>%s", codec->name, codec->long_name);
    //��ȡ������������ģ�û����ΪɶҪ��ȡһ��AVCodecContext������������AVCodecParameters����
    /*
    AVCodecParameters��AVCodecContext������
    AVCodecContext�ṹ����һ���������Ľṹ�壬������AVCodecParameters�ṹ��������Ϣ��ͬʱ��������һЩAVCodecParameters��û�еĲ���������һЩ����������صĿ�����Ϣ��
    AVCodecContext�ṹ��ͨ��������������Ƶ�ı�������������ڽ���ͱ���Ȳ�����
    �����н���ʱ��ͨ���ȴ�AVCodecParameters�ṹ���д���һ��AVCodecContext�ṹ�壬��ʹ�øýṹ����н��롣
    ��ʹ��AVCodecParameters��AVCodecContextʱ��������Ҫ��ȷ����֮��Ĺ�ϵ��AVCodecContext�ṹ����AVCodecParameters�ṹ���һ������.
    AVCodecParameters�ṹ���а�����AVCodecContext�ṹ���еĴ󲿷ֲ���������AVCodecContext�ṹ���л������˺ܶ�AVCodecParametersû�еı��������ز�����
    ���ԣ���һЩ���������йصĲ����У�������Ҫʹ��AVCodecContext�����ڽ�����ȡ����Ƶ���Ĳ�����Ϣʱ�����ǿ���ʹ��AVCodecParameters��
    */
    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == NULL) {
        hloge("avcodec_alloc_context3");
        ret = -40;
        return ret;
    }
    defer (if (ret != 0 && codec_ctx) {avcodec_free_context(&codec_ctx); codec_ctx = NULL;})
    //��AVCodecParameters�ṹ������������������AVCodecContext�ṹ����
    ret = avcodec_parameters_to_context(codec_ctx, codec_param);
    if (ret != 0) {
        hloge("avcodec_parameters_to_context error: %d", ret);
        return ret;
    }
    //����������ɶ��˼������
    //(1)��AVCodecContext.refcounted_rames������Ϊ1����AVFrame�����ü��������ص��������ڵ����ߡ�
    //��������ҪAVFrameʱ�������߱���ʹ��av_frame_unref()���ͷ�frame��ֻ����av_frame_is_writable()����1�������߲ſ�����frame��д�����ݡ�
    //(2)��AVCodecContextrefcounted_frames ������Ϊ0�����ص��������ڽ�������ֻ������һ�ε��øú�����رջ�ˢ�½�����֮ǰ��Ч�������߲�����AVFrame ��д�����ݡ�
    if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
    }
    //�򿪱������
    ret = avcodec_open2(codec_ctx, codec, &codec_opts);
    //�����ʧ���ˣ����������ɣ���������в���
    if (ret != 0) {
        if (real_decode_mode != SOFTWARE_DECODE) {
            hlogi("Can not open hardwrae codec error: %d, try software codec.", ret);
            goto try_software_decode;
        }
        hloge("Can not open software codec error: %d", ret);
        return ret;
    }
    // ѡ������Щpackets,Ĭ��ֻ��sizeΪ0��
    video_stream->discard = AVDISCARD_DEFAULT;
    //һЩ������ֵ����ߣ����ظ�ʽ��dw��dh�Ƕ����Ŀ�ߣ���Ϊ�����ʽ������ڶ���16*16�ģ���������ܻᵼ���̱ߡ�
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
    //Ŀ�����ظ�ʽ
    dst_pix_fmt = AV_PIX_FMT_YUV420P;
    //���������ļ���ȷ�������Ŀ�����ظ�ʽ
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
    //��ʼ��ת������
    sws_ctx = sws_getContext(sw, sh, src_pix_fmt, dw, dh, dst_pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
    if (sws_ctx == NULL) {
        hloge("sws_getContext");
        ret = -50;
        return ret;
    }
    //�м�packet��frame�����ù�
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    //��ֵ
    hframe.w = dw;
    hframe.h = dh;
    // ARGB,��ȷ��ʲô��ʽ���ȿ��ٸ���Ŀռ䱣֤�϶��ŵ���
    hframe.buf.resize(dw * dh * 4);
    //���ݾ�������ظ�ʽ���и�ֵ��data��linesize���÷�ֵ��Ʒζ
    //����Ϊɶ������Ϳ��Զ���������Ϊavpacket��ѹ����ģ�avframeûѹ����ȷ����߸�ʽ֮���֪���ռ��С��
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
    //�ܺ���⣬��һ��fps�Ƕ��ٶ���
    if (video_stream->avg_frame_rate.num && video_stream->avg_frame_rate.den) {
        fps = video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    }
    //��ԭʼ�Ŀ��(û�����)�Ĵ���һ�£���֪���������������ǳ�ʼ����һЩ����
    width = sw;
    height = sh;
    duration = 0;
    start_time = 0;
    eof = 0;
    error = 0;
    //��һ����Ƶ��ʱ��
    if (video_time_base_num && video_time_base_den) {
        if (video_stream->duration > 0) {
            duration = video_stream->duration / (double)video_time_base_den * video_time_base_num * 1000;
        }
        if (video_stream->start_time > 0) {
            start_time = video_stream->start_time / (double)video_time_base_den * video_time_base_num * 1000;
        }
    }
    hlogi("fps=%d duration=%lldms start_time=%lldms", fps, duration, start_time);
    //�������Ƶ������Ҫ�����ǾͰ��ղ��Ĳ�������Ƶ����Ҳ��ʼ��һ��,������ֱ�Ӹ���ret��һ������
    if(ctrl)
        if(ret==0)
            ret=open_audio();
        else
            open_audio();
    //׼�����������ˣ�����һ���߳̽���һ��Ҫ˯���
    //HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000 / fps);
    HThread::setSleepPolicy(HThread::SLEEP_FOR,0);
    return ret;
}
int HFFPlayer::open_audio(){
    int ret=0;
    //��ȡ��Ƶ��
    AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
    //������Ƶ���Ļ���������Ϣ��ȡ����֮ǰ��AVCodecContext
    /*codectype������������ͣ�ȡֵΪAVMEDIATYPEVIDEO��AVMEDIATYPEAUDIO��AVMEDIATYPE_SUBTITLE֮һ��
     codec_id�����������ID��ȡֵΪö������AVCodecID�е�һ�֡�
     codec_tag�����������ǩ��
     extradata������ĳЩ����������H.264����Ҫ�������Ϣ�����룬��Щ��Ϣ��Ϊ��extradata�����ó�Ա������������Щ������Ϣ�ĵ�ַ��
     extradata_size��extradata��ռ�õ��ֽ�����
     format������Ƶ֡�����ػ������ʽ�����������Ƶ��˵��������AVPIXFMTYUV420P��AVPIXFMTNV12�ȵȡ�������Ƶ��˵��������AVSAMPLEFMTS16��AVSAMPLEFMTFLTP�ȵȡ�
     bit_rate�������ʣ�������������Ƶ������������λΪbps��
     bitspercoded_sample��ÿ�������������ռ�õ�λ����
     bitsperraw_sample��ÿ��������������ռ�õ�λ����
     profile�����������Э��ȼ���
     level�����������Э��ȼ���
     width����Ƶ֡��ȡ�
     height����Ƶ֡�߶ȡ�
     sample_rate����Ƶ�����ʡ�
     channels����Ƶ��������
     samplefmt����Ƶ������ʽ������AVSAMPLEFMTS16��AVSAMPLEFMT_FLTP�ȵȡ�
     channellayout����Ƶ�������֣�����AVCHLAYOUTMONO��AVCHLAYOUT_STEREO�ȵȡ�
     sampleaspectratio������Ƶ֡��߱ȡ�
     fpsfirstnum��֡�ʷ��ӡ�
     fpssecondnum��֡�ʷ�ĸ��
     delay���������ı���ʱ�ӡ�
     seek_preroll�������Ҫ���йؼ�֡����ת������ֶα�ʾ��Ҫ������֡����
*/
    AVCodecParameters* acodec_param = audio_stream->codecpar;
    audio_time_base_num=audio_stream->time_base.num;
    audio_time_base_den=audio_stream->time_base.den;
    hlogi("codec_id=%d:%s", acodec_param->codec_id, avcodec_get_name(acodec_param->codec_id));
    //��Ƶ�������������˱�����������͡����ơ�֧�ֵ�����Ƶ��ʽ������뺯���ȡ�ͨ��AVCodec�ṹ�壬���Բ�ѯ�ͻ�ȡϵͳ�п��õı������������AVCodecParameters�����Խ�������Ƶ����������
    AVCodec* codec = NULL;
    codec = avcodec_find_decoder(acodec_param->codec_id);
    hlogi("codec_name: %s=>%s", codec->name, codec->long_name);
    //��ȡ������������ģ�û����ΪɶҪ��ȡһ��AVCodecContext������������AVCodecParameters����
    /*
    AVCodecParameters��AVCodecContext������
    AVCodecContext�ṹ����һ���������Ľṹ�壬������AVCodecParameters�ṹ��������Ϣ��ͬʱ��������һЩAVCodecParameters��û�еĲ���������һЩ����������صĿ�����Ϣ��
    AVCodecContext�ṹ��ͨ��������������Ƶ�ı�������������ڽ���ͱ���Ȳ�����
    �����н���ʱ��ͨ���ȴ�AVCodecParameters�ṹ���д���һ��AVCodecContext�ṹ�壬��ʹ�øýṹ����н��롣
    ��ʹ��AVCodecParameters��AVCodecContextʱ��������Ҫ��ȷ����֮��Ĺ�ϵ��AVCodecContext�ṹ����AVCodecParameters�ṹ���һ������.
    AVCodecParameters�ṹ���а�����AVCodecContext�ṹ���еĴ󲿷ֲ���������AVCodecContext�ṹ���л������˺ܶ�AVCodecParametersû�еı��������ز�����
    ���ԣ���һЩ���������йصĲ����У�������Ҫʹ��AVCodecContext�����ڽ�����ȡ����Ƶ���Ĳ�����Ϣʱ�����ǿ���ʹ��AVCodecParameters��
    */
    acodec_ctx = avcodec_alloc_context3(codec);
    if (acodec_ctx == NULL) {
        hloge("avcodec_alloc_context3-audio");
        ret = -40-100;
        ctrl=0;
        return ret;
    }
    defer (if (ret != 0 && acodec_ctx) {avcodec_free_context(&acodec_ctx); acodec_ctx = NULL;})
    //��AVCodecParameters�ṹ������������������AVCodecContext�ṹ����
    ret = avcodec_parameters_to_context(acodec_ctx, acodec_param);
    if (ret != 0) {
        hloge("avcodec_parameters_to_context error-audio: %d", ret);
        ctrl=0;
        return ret;
    }
    //����������ɶ��˼������
    //(1)��AVCodecContext.refcounted_rames������Ϊ1����AVFrame�����ü��������ص��������ڵ����ߡ�
    //��������ҪAVFrameʱ�������߱���ʹ��av_frame_unref()���ͷ�frame��ֻ����av_frame_is_writable()����1�������߲ſ�����frame��д�����ݡ�
    //(2)��AVCodecContextrefcounted_frames ������Ϊ0�����ص��������ڽ�������ֻ������һ�ε��øú�����رջ�ˢ�½�����֮ǰ��Ч�������߲�����AVFrame ��д�����ݡ�
    if (acodec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || acodec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
    }
    //�򿪱������
    ret = avcodec_open2(acodec_ctx, codec, &codec_opts);
    //�����ʧ���ˣ�����
    if (ret != 0) {
        hloge("Can not open software codec error-audio: %d", ret);
        ctrl=0;
        return ret;
    }
    // ѡ������Щpackets,Ĭ��ֻ��sizeΪ0��
    audio_stream->discard = AVDISCARD_DEFAULT;
    //һЩ����
    audio_src_smft = acodec_ctx->sample_fmt;
    //Ŀ���ʽ
    audio_dst_smft = AV_SAMPLE_FMT_S16;
    //���������ļ���ȷ�������Ŀ�����ظ�ʽ
    std::string str = g_confile->GetValue("dst_smft", "audio");
    if (!str.empty()) {
        if (strcmp(str.c_str(), "AV_SAMPLE_FMT_S16") == 0) {
            audio_dst_smft = AV_SAMPLE_FMT_S16;
        }
        else
            audio_dst_smft = AV_SAMPLE_FMT_S32;
    }
    hlogi("audio_dst_smft=%d", audio_dst_smft);
    //��ʼ��ת������
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
    //��ֵ
    aframe.chanels = acodec_ctx->channels;
    aframe.sample_rate = acodec_ctx->sample_rate;
    // ��ȷ��ʲô��ʽ���ȿ��ٸ���Ŀռ䱣֤�϶��ŵ���
    aframe.buf.resize(acodec_ctx->frame_size * acodec_ctx->channels * 32);
    //���ݾ���ĸ�ʽ���и�ֵ��data��linesize���÷�ֵ��Ʒζ
    //����Ϊɶ������Ϳ��Զ���������Ϊavpacket��ѹ����ģ�avframeûѹ����ȷ����ʽ֮���֪���ռ��С��
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
