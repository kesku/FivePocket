// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "audio_capture_writer.h"
#include "storage_space.h"
#include <string>
#include <string_view>

namespace recomp::apple {
inline unsigned audioCaptureSeconds(std::string_view value) noexcept {
    if(value.empty() || value.size()>3 || value.front()=='0')return 0;
    unsigned seconds=0;
    for(char c:value){if(c<'0' || c>'9')return 0;seconds=seconds*10+unsigned(c-'0');}
#if defined(APPLE_CAPTURE_MUTANT_DURATION)
    return seconds;
#else
    return seconds<=480?seconds:0;
#endif
}

// Secure, exclusive output below a caller-owned diagnostic directory. The
// production caller chooses the root; iOS accepts no environment-provided path.
// No directory/file is removed, even on errors. No writes to assets or saves.
class AudioCaptureSession final {
    std::unique_ptr<AudioCaptureWriter> writer_;
public:
    AudioCaptureSession(const char* root,std::string_view nonce,unsigned seconds) {
        if(!seconds || seconds>480 || nonce.size()!=32 ||
           nonce.find_first_not_of("0123456789abcdef")!=std::string_view::npos)
            throw std::invalid_argument("bounded capture and exact nonce required");
        const auto frames=std::uint64_t(seconds)*48000;
        const auto space=storageSpace(root);
        if(!space.valid || space.availableBytes<frames*8+64ull*1024*1024)
            throw std::runtime_error("capture free-space snapshot insufficient");
        // Finish allocation before acquiring the raw directory descriptor.
        const std::string name(nonce);
        const int parent=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(parent<0)throw std::runtime_error("capture root unavailable");
        const bool created=mkdirat(parent,name.c_str(),0700)==0;
        const int directory=created?openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
        ::close(parent);
        if(directory<0)throw std::runtime_error("exclusive capture directory unavailable");
        const int fd=openat(directory,"output.f32le",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        ::close(directory);
        if(fd<0)throw std::runtime_error("exclusive capture payload unavailable");
        // Allocate before opening would simplify ownership, but new can fail
        // before the writer constructor takes the fd. Separate allocation from
        // construction so this rare error still closes the descriptor.
        void* storage=nullptr;
        try {storage=::operator new(sizeof(AudioCaptureWriter),std::align_val_t(alignof(AudioCaptureWriter)));}
        catch(...){::close(fd);throw;}
        try {writer_.reset(new(storage) AudioCaptureWriter(fd,frames));}
        catch(...){::operator delete(storage,std::align_val_t(alignof(AudioCaptureWriter)));throw;}
    }
    void push(const float* data,std::uint32_t frames) noexcept {writer_->push(data,frames);}
    void finishAfterProducerStopped() noexcept {writer_->finishAfterProducerStopped();}
    bool result(AudioCaptureWriter::Result& out)const noexcept{return writer_->result(out);}
};
}
