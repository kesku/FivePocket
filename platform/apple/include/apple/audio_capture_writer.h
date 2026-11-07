// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "audio_capture_queue.h"
#include <bit>
#include <cerrno>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace recomp::apple {
// Owns a caller-created exclusive, private, empty regular-file descriptor.
// The caller must stop AudioUnit callbacks before destruction. The worker alone
// performs disk I/O. Failure/truncation is evidence failure, never playback error.
class AudioCaptureWriter final {
public:
    static constexpr std::uint64_t MaximumFrames=48000ull*480;
    struct Result {std::uint64_t capturedFrames=0,writtenFrames=0,writtenBytes=0;int error=0;
                   bool overflow=false,complete=false;};
private:
    static_assert(std::endian::native==std::endian::little);
    AudioCaptureQueue<131072> queue_;
    const std::uint64_t limit_;
    int fd_=-1;
    std::thread worker_;
    Result result_{};
    std::atomic<bool> done_{false};
    void work() noexcept {
        std::array<float,8192> buffer{};
        while(!queue_.finished() || queue_.read()!=queue_.written()) {
            const auto frames=queue_.pop(buffer.data(),4096);
            if(!frames) {std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
            const auto* bytes=reinterpret_cast<const unsigned char*>(buffer.data());
            const std::size_t count=std::size_t(frames)*8;
            std::size_t offset=0;
            while(offset<count) {
                const auto n=::write(fd_,bytes+offset,count-offset);
                if(n<0 && errno==EINTR)continue;
                if(n<=0) {result_.error=n<0?errno:EIO;break;}
                offset+=std::size_t(n);
                result_.writtenBytes+=std::size_t(n);
            }
            if(result_.error)break;
            result_.writtenFrames+=frames;
        }
        result_.capturedFrames=queue_.written();
        result_.overflow=queue_.overflowed();
        if(!result_.error && fsync(fd_)!=0)result_.error=errno;
        const int closeStatus=::close(fd_);fd_=-1;
        if(closeStatus!=0 && !result_.error)result_.error=errno;
        result_.complete=!result_.error && !result_.overflow && queue_.complete() &&
                         result_.writtenFrames==limit_;
#if defined(APPLE_CAPTURE_MUTANT_IGNORE_IO)
        if(result_.error){result_.error=0;result_.complete=true;}
#endif
        done_.store(true,std::memory_order_release);
    }
public:
    // Throws before starting if validation fails. Ownership transfers on entry,
    // including failure. No existing data may be overwritten through this API.
    AudioCaptureWriter(int fd,std::uint64_t frames) : queue_(frames),limit_(frames),fd_(fd) {
        struct stat st{};
        const int flags=fd_>=0?fcntl(fd_,F_GETFL):-1;
        if(fd_<0 || !frames || frames>MaximumFrames || fstat(fd_,&st)!=0 ||
           !S_ISREG(st.st_mode) || st.st_size || st.st_nlink!=1 || (st.st_mode&0077) ||
           lseek(fd_,0,SEEK_CUR)!=0 || flags<0 || (flags&O_ACCMODE)==O_RDONLY) {
            if(fd_>=0)::close(fd_);fd_=-1;
            throw std::invalid_argument("exclusive empty regular capture file and bounded duration required");
        }
        try {worker_=std::thread([this]{work();});}
        catch(...) {::close(fd_);fd_=-1;throw;}
    }
    AudioCaptureWriter(const AudioCaptureWriter&)=delete;
    AudioCaptureWriter& operator=(const AudioCaptureWriter&)=delete;
    ~AudioCaptureWriter() {finishAfterProducerStopped();}
    void push(const float* output,std::uint32_t frames) noexcept {queue_.push(output,frames);}
    void finishAfterProducerStopped() noexcept {
        queue_.finishAfterProducerStopped();
        if(worker_.joinable())worker_.join();
    }
    bool result(Result& out) const noexcept {
        if(!done_.load(std::memory_order_acquire))return false;
        out=result_;return true;
    }
};
}
