#ifndef HV_FRAME_H_
#define HV_FRAME_H_

#include <deque>
#include <mutex>

#include "hbuf.h"

class HFrame {
public:
    HBuf buf;
    int w;
    int h;
    int bpp;
    int type;
    uint64_t ts;
    int64_t useridx;
    void* userdata;

    HFrame() {
        w = h = bpp = type = 0;
        ts = 0;
        useridx = -1;
        userdata = NULL;
    }

    bool isNull() {
        return w == 0 || h == 0 || buf.isNull();
    }

    void copy(const HFrame& rhs) {
        w = rhs.w;
        h = rhs.h;
        bpp = rhs.bpp;
        type = rhs.type;
        ts = rhs.ts;
        useridx = rhs.useridx;
        userdata = rhs.userdata;
        buf.copy(rhs.buf.base, rhs.buf.len);
    }
};

class HAFrame {
public:
    HBuf buf;
    int chanels;
    int sample_rate;
    int type;
    int bpp;
    uint64_t ts;
    int64_t useridx;
    void* userdata;
    int duration;

    HAFrame() {
        duration=chanels =bpp= sample_rate = type = 0;
        ts = 0;
        useridx = -1;
        userdata = NULL;
    }

    bool isNull() {
        return chanels == 0 || sample_rate == 0 || buf.isNull();
    }

    void copy(const HAFrame& rhs) {
        chanels = rhs.chanels;
        sample_rate = rhs.sample_rate;
        type = rhs.type;
        ts = rhs.ts;
        useridx = rhs.useridx;
        userdata = rhs.userdata;
        duration = rhs.duration;
        buf.copy(rhs.buf.base, rhs.buf.len);
    }
};
typedef struct frame_info_s {
    int w;
    int h;
    int type;
    int bpp;
    int chanels;
    int sample_rate;
} FrameInfo;

typedef struct frame_stats_s {
    int push_cnt;
    int pop_cnt;

    int push_ok_cnt;
    int pop_ok_cnt;
    int size;
    frame_stats_s() {
        size=push_cnt = pop_cnt = push_ok_cnt = pop_ok_cnt = 0;
    }
} FrameStats;

#define DEFAULT_FRAME_CACHENUM  10

class HFrameBuf : public HRingBuf {
 public:
    enum CacheFullPolicy {
        SQUEEZE,
        DISCARD,
    } policy;

    HFrameBuf() : HRingBuf() {
        cache_num = DEFAULT_FRAME_CACHENUM;
        policy = SQUEEZE;
    }

    void setCache(int num) {cache_num = num;}
    void setPolicy(CacheFullPolicy policy) {this->policy = policy;}

    int push(HFrame* pFrame);
    int pop(HFrame* pFrame);
    void clear();

    int         cache_num;
    FrameStats  frame_stats;
    FrameInfo   frame_info;
    std::deque<HFrame> frames;
    std::mutex         mutex;
};
class AFrameBuf : public HRingBuf {
 public:
    enum CacheFullPolicy {
        SQUEEZE,
        DISCARD,
    } policy;

    AFrameBuf() : HRingBuf() {
        cache_num = DEFAULT_FRAME_CACHENUM;
        policy = SQUEEZE;
    }

    void setCache(int num) {cache_num = num;}
    void setPolicy(CacheFullPolicy policy) {this->policy = policy;}

    int push(HAFrame* pFrame);
    int pop(HAFrame* pFrame);
    void clear();

    int         cache_num;
    FrameStats  frame_stats;
    FrameInfo   frame_info;
    std::deque<HAFrame> frames;
    std::mutex         mutex;
};
#endif // HV_FRAME_H_
