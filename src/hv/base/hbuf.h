#ifndef HV_BUF_H_
#define HV_BUF_H_

/*
 * @���ܣ���ͷ�ļ��ṩ��һЩ���õ�buffer
 *
 */

#include "hdef.h"   // for MAX
#include "hbase.h"  // for HV_ALLOC, HV_FREE

typedef struct hbuf_s {
    char*  base;
    size_t len;

#ifdef __cplusplus
    hbuf_s() {
        base = NULL;
        len  = 0;
    }

    hbuf_s(void* data, size_t len) {
        this->base = (char*)data;
        this->len  = len;
    }
#endif
} hbuf_t;

// offset_buf_t ����һ��offsetƫ������Ա������
// ͨ������һ��δ�����꣬��¼��һ�β��������
typedef struct offset_buf_s {
    char*   base;
    size_t  len;
    size_t  offset;
#ifdef __cplusplus
    offset_buf_s() {
        base = NULL;
        len = offset = 0;
    }

    offset_buf_s(void* data, size_t len) {
        this->base = (char*)data;
        this->len = len;
    }
#endif
} offset_buf_t;

#ifdef __cplusplus
class HBuf : public hbuf_t {
public:
    HBuf() : hbuf_t() {
        cleanup_ = false;
    }
    // ǳ�������캯��
    HBuf(void* data, size_t len) : hbuf_t(data, len) {
        cleanup_ = false;
    }
    HBuf(size_t cap) { resize(cap); }

    virtual ~HBuf() {
        cleanup();
    }

    void*  data() { return base; }
    size_t size() { return len; }

    bool isNull() { return base == NULL || len == 0; }

    void cleanup() {
        if (cleanup_) {
            HV_FREE(base);
            len = 0;
            cleanup_ = false;
        }
    }

    void resize(size_t cap) {
        if (cap == len) return;

        if (base == NULL) {
            HV_ALLOC(base, cap);
        }
        else {
            base = (char*)safe_realloc(base, cap, len);
        }
        len = cap;
        cleanup_ = true;
    }

    // ���
    void copy(void* data, size_t len) {
        resize(len);
        memcpy(base, data, len);
    }

    void copy(hbuf_t* buf) {
        copy(buf->base, buf->len);
    }

private:
    bool cleanup_; // cleanup_����������¼buf��ǳ���������������������������ʱ���ͷŵ������ڴ�
};

// �ɱ䳤buffer���ͣ�֧��push_front/push_back/pop_front/pop_back����
// VL: Variable-Length
class HVLBuf : public HBuf {
public:
    HVLBuf() : HBuf() {_offset = _size = 0;}
    HVLBuf(void* data, size_t len) : HBuf(data, len) {_offset = 0; _size = len;}
    HVLBuf(size_t cap) : HBuf(cap) {_offset = _size = 0;}
    virtual ~HVLBuf() {}

    // ���ص�ǰ���
    char* data() { return base + _offset; }
    // ������Ч����
    size_t size() { return _size; }

    void push_front(void* ptr, size_t len) {
        // ������볤�ȳ�����ʣ��ռ䣬�����·����㹻�ռ�
        if (len > this->len - _size) {
            size_t newsize = MAX(this->len, len)*2;
            base = (char*)safe_realloc(base, newsize, this->len);
            this->len = newsize;
        }

        // ���ǰ��ռ䲻�㣬����Ҫ���������
        if (_offset < len) {
            // move => end
            memmove(base+this->len-_size, data(), _size);
            _offset = this->len-_size;
        }

        // ���뵽��ǰ����ǰ��
        memcpy(data()-len, ptr, len);
        // ��¼�µ����λ��
        _offset -= len;
        // ��Ч��������
        _size += len;
    }

    void push_back(void* ptr, size_t len) {
        // ������볤�ȳ�����ʣ��ռ䣬�����·����㹻�ռ�
        if (len > this->len - _size) {
            size_t newsize = MAX(this->len, len)*2;
            base = (char*)safe_realloc(base, newsize, this->len);
            this->len = newsize;
        }
        // �������ռ䲻�㣬����Ҫ������ǰ��
        else if (len > this->len - _offset - _size) {
            // move => start
            memmove(base, data(), _size);
            _offset = 0;
        }
        // ���뵽����
        memcpy(data()+_size, ptr, len);
        // ���λ�ò��䣬��Ч��������
        _size += len;
    }

    void pop_front(void* ptr, size_t len) {
        if (len <= _size) {
            // �����ݴӿ�ʼλ�ÿ�������
            if (ptr) {
                memcpy(ptr, data(), len);
            }
            // ���λ�ú���
            _offset += len;
            // ������λ���Ѿ����˽�β��������Ϊ0
            if (_offset >= this->len) _offset = 0;
            // ��Ч���ȼ���
            _size   -= len;
        }
    }

    void pop_back(void* ptr, size_t len) {
        if (len <= _size) {
            // �����ݴ�β����������
            if (ptr) {
                memcpy(ptr, data()+_size-len, len);
            }
            // ���λ�ò��䣬��Ч���ȼ���
            _size -= len;
        }
    }

    void clear() {
        // ��������������λ�ú���Ч��������Ϊ0
        _offset = _size = 0;
    }

    // һЩ��������
    void prepend(void* ptr, size_t len) {
        push_front(ptr, len);
    }

    void append(void* ptr, size_t len) {
        push_back(ptr, len);
    }

    void insert(void* ptr, size_t len) {
        push_back(ptr, len);
    }

    void remove(size_t len) {
        pop_front(NULL, len);
    }

private:
    size_t _offset; // _offet������¼��ǰ����ƫ����
    size_t _size; // _size������¼��Ч����
};

// ����buffer������ӻ���buffer�з������ͷ��ڴ棬����Ƶ������ϵͳ����
class HRingBuf : public HBuf {
public:
    HRingBuf() : HBuf() {_head = _tail = _size = 0;}
    HRingBuf(size_t cap) : HBuf(cap) {_head = _tail = _size = 0;}
    virtual ~HRingBuf() {}

    char* alloc(size_t len) {
        char* ret = NULL;
        // ���ͷָ����βָ��ǰ��������ó��ȵ���0
        if (_head < _tail || _size == 0) {
            // [_tail, this->len) && [0, _head)
            // ���βָ�����ʣ��ռ��㹻�����βָ���ʼ����ռ�
            if (this->len - _tail >= len) {
                ret = base + _tail;
                _tail += len;
                if (_tail == this->len) _tail = 0;
            }
            // ���ͷָ��ǰ��ʣ��ռ��㹻�����0��ʼ����ռ�
            else if (_head >= len) {
                ret = base;
                _tail = len;
            }
        }
        else {
            // [_tail, _head)
            // ���βָ�뵽ͷָ���Ŀռ��㹻�����βָ���ʼ����ռ�
            if (_head - _tail >= len) {
                ret = base + _tail;
                _tail += len;
            }
        }
        // ���䵽�˿ռ䣬���ó�������
        _size += ret ? len : 0;
        return ret;
    }

    void free(size_t len) {
        // ���ó��ȼ���
        _size -= len;
        // ����ͷŵĳ���С��ͷָ�����ĳ��ȣ�ͷָ����ƣ�
        if (len <= this->len - _head) {
            _head += len;
            if (_head == this->len) _head = 0;
        }
        // ����˵��ͷָ���ѵ�β��������ͷŵ��ڴ��Ǵ�0��ʼ����ģ�ͷָ����Ϊlen����,��Ϊÿ�η����ڴ泤�ȵĴ�С��һ���ģ����Ժ��治����һ���Ǵ�0��ʼ
        else {
            _head = len;
        }
    }

    void clear() {_head = _tail = _size = 0;}

    size_t size() {return _size;}

private:
    size_t _head; // ͷָ�룬������¼��λ��
    size_t _tail; // βָ�룬������¼дλ��
    size_t _size; // ������¼���ó���
};
#endif

#endif // HV_BUF_H_
