#ifndef MsgChannel_hpp
#define MsgChannel_hpp

#include <iostream>
#include <deque>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>

/* Implementation of the MsgChannel API, except that it keeps
 * the most recent bsize elements in the queue.
 */
template <class T> class MsgChannel {
public:
    MsgChannel(unsigned int bsize) : buffer_size{bsize} { }

    bool write(T msg) {
        std::unique_lock<std::mutex> lck(cv_mtx);
        msg_buffer.push_front(std::move(msg));
        bool overflowed = false;
        if (msg_buffer.size() > buffer_size) {
            msg_buffer.pop_back();
            overflowed = true;
        }
        write_cv.notify_all();
        return !overflowed;
    }

    bool read(T *out) {
        std::unique_lock<std::mutex> lck(cv_mtx);
        if (can_read()) {
            *out = std::move(msg_buffer.back());
            msg_buffer.pop_back();
            return true;
        }
        return false;
    }

    bool peek_oldest(T *out) {
        std::unique_lock<std::mutex> lck(cv_mtx);
        if (can_read()) {
            *out = msg_buffer.back();
            return true;
        }
        return false;
    }

    T wait_read() {
        std::unique_lock<std::mutex> lck(cv_mtx);
        while (!can_read()) {
            write_cv.wait(lck);
        };
        T val = std::move(msg_buffer.back());
        msg_buffer.pop_back();
        return val;
    }

    template <class Rep, class Period>
    bool wait_read_for(T *out, const std::chrono::duration<Rep, Period> &timeout) {
        std::unique_lock<std::mutex> lck(cv_mtx);
        if (!write_cv.wait_for(lck, timeout, [&] { return can_read(); })) {
            return false;
        }
        *out = std::move(msg_buffer.back());
        msg_buffer.pop_back();
        return true;
    }

    void clear() {
        std::unique_lock<std::mutex> lck(cv_mtx);
        msg_buffer.clear();
    }

    size_t size() {
        std::unique_lock<std::mutex> lck(cv_mtx);
        return msg_buffer.size();
    }

    size_t discard_oldest(size_t count) {
        std::unique_lock<std::mutex> lck(cv_mtx);
        size_t dropped = 0;
        while (dropped < count && can_read()) {
            msg_buffer.pop_back();
            ++dropped;
        }
        return dropped;
    }

private:
    bool can_read() {
        return !msg_buffer.empty();
    }

    std::deque<T> msg_buffer;
    std::mutex cv_mtx;
    std::condition_variable write_cv;
    unsigned int buffer_size;
};

#endif
