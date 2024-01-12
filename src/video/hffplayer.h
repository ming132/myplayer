#ifndef H_FFPLAYER_H
#define H_FFPLAYER_H

#include "HVideoPlayer.h"
#include "ffmpeg_util.h"
#include "hthread.h"

#include <atomic>

class HFFPlayer : public HVideoPlayer, public HThread {
public:
    HFFPlayer();
    ~HFFPlayer();

    virtual int start() {
        quit = 0;
        return HThread::start();
    }
    virtual int stop() {
        quit = 1;
        return HThread::stop();
    }
    virtual int pause() {return HThread::pause();}
    virtual int resume() {return HThread::resume();}

    virtual int seek(int64_t ms);

private:
    virtual bool doPrepare();
    virtual void doTask();
    virtual bool doFinish();

    int open();
    int open_audio();
    int close();

public:
    int64_t block_starttime;
    int64_t block_timeout;
    int     quit;

private:
    static std::atomic_flag s_ffmpeg_init;

    AVDictionary*       fmt_opts;
    AVDictionary*       codec_opts;

    AVFormatContext*    fmt_ctx;
    AVCodecContext*     codec_ctx;
    AVCodecContext*     acodec_ctx;

    AVPacket* packet;
    AVFrame* frame;

    int video_stream_index;
    int audio_stream_index;
    int subtitle_stream_index;

    int video_time_base_num;
    int video_time_base_den;
    int audio_time_base_num;
    int audio_time_base_den;

    // for scale
    AVPixelFormat   src_pix_fmt;
    AVPixelFormat   dst_pix_fmt;
    AVSampleFormat  audio_src_smft;
    AVSampleFormat  audio_dst_smft;
    SwsContext*     sws_ctx;
    SwrContext*     swr_ctx;
    uint8_t*        data[4];
    int             linesize[4];
    HFrame          hframe;
    HAFrame         aframe;
    uint8_t*        audio_data[4];
    int             audio_linesize[4];
    int ctrl;
};

#endif // H_FFPLAYER_H
