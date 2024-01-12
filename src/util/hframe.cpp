#include "hframe.h"
#include "iostream"
#include "hlog.h"

int HFrameBuf::push(HFrame* pFrame) {
    if (pFrame->isNull())
        return -10;

    frame_stats.push_cnt++;

    std::lock_guard<std::mutex> locker(mutex);

    if (frames.size() >= (size_t)cache_num) {
        hlogd("frame cache full!");
        if (policy == HFrameBuf::DISCARD) {
            return -20;     // note: cache full, discard frame
        }

        HFrame& frame = frames.front();
        frames.pop_front();
        free(frame.buf.len);
        if (frame.userdata) {
            hlogd("free userdata");
            ::free(frame.userdata);
            frame.userdata = NULL;
        }
        frame_stats.size--;
    }

    int ret = 0;
    if (isNull()) {
        resize(pFrame->buf.len * cache_num);
        ret = 1;    // note: first push

        frame_info.w = pFrame->w;
        frame_info.h = pFrame->h;
        frame_info.type = pFrame->type;
        frame_info.bpp  = pFrame->bpp;
    }

    HFrame frame;
    frame.buf.base = alloc(pFrame->buf.len);
    frame.buf.len  = pFrame->buf.len;
    frame.copy(*pFrame);
    frames.push_back(frame);
    frame_stats.push_ok_cnt++;
    frame_stats.size++;
    return ret;
}
int AFrameBuf::push(HAFrame* pFrame) {
    if (pFrame->isNull())
        return -10;

    frame_stats.push_cnt++;

    std::lock_guard<std::mutex> locker(mutex);

    if (frames.size() >= (size_t)cache_num) {
        hlogd("frame cache full!");
        if (policy == HFrameBuf::DISCARD) {
            return -20;     // note: cache full, discard frame
        }

        HAFrame& frame = frames.front();
        frames.pop_front();
        free(frame.buf.len);
        if (frame.userdata) {
            hlogd("free userdata");
            ::free(frame.userdata);
            frame.userdata = NULL;
            frame_stats.size--;
        }
    }

    int ret = 0;
    if (isNull()) {
        resize(pFrame->buf.len * cache_num);
        ret = 1;    // note: first push

        frame_info.sample_rate = pFrame->sample_rate;
        frame_info.chanels = pFrame->chanels;
        frame_info.type = pFrame->type;
        frame_info.bpp  = pFrame->bpp;
    }

    HAFrame frame;
    frame.buf.base = alloc(pFrame->buf.len);
    frame.buf.len  = pFrame->buf.len;
    frame.copy(*pFrame);
    frames.push_back(frame);
    frame_stats.push_ok_cnt++;
    frame_stats.size++;
    return ret;
}
int HFrameBuf::pop(HFrame* pFrame) {
    frame_stats.pop_cnt++;

    std::lock_guard<std::mutex> locker(mutex);

    if (isNull())
        return -10;

    if (frames.size() == 0) {
        hlogd("frame cache empty!");
        return -20;
    }

    HFrame& frame = frames.front();
    frames.pop_front();
    free(frame.buf.len);

    if (frame.isNull())
        return -30;

    pFrame->copy(frame);
    frame_stats.pop_ok_cnt++;
    frame_stats.size--;
    //std::cout<<"vedio_frame_size: "<<frame_stats.size<<"\n";
    return 0;
}

int AFrameBuf::pop(HAFrame* pFrame) {
    frame_stats.pop_cnt++;

    std::lock_guard<std::mutex> locker(mutex);

    if (isNull())
        return -10;

    if (frames.size() == 0) {
        hlogd("frame cache empty!");
        return -20;
    }

    HAFrame& frame = frames.front();
    frames.pop_front();
    free(frame.buf.len);

    if (frame.isNull())
        return -30;

    pFrame->copy(frame);
    frame_stats.pop_ok_cnt++;
    frame_stats.size--;
    //std::cout<<"audio_frame_size: "<<frame_stats.size<<"\n";
    return 0;
}
void AFrameBuf::clear() {
    std::lock_guard<std::mutex> locker(mutex);
    frames.clear();
    frame_stats.size=0;
    HRingBuf::clear();
}
void HFrameBuf::clear() {
    std::lock_guard<std::mutex> locker(mutex);
    frames.clear();
    frame_stats.size=0;
    HRingBuf::clear();
}
