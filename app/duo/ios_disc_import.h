// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios_disc_import.h (Claude 2026-11-05; the builder's iOS brief, common work item 9): the on-device disc import of the iOS app, a port
// of android/'s pure-Java import line for line - Xdvdfs, Disc2ImageProbe, Disc2Window, Disc2Stfs, Disc2Discs (open / plan / judge /
// installed / missing), SetupCatalog (the 18 pinned files: the play disc's 10 and the install disc's 8) and SetupDiscCopy with the
// VerifiedExtractor safeguards it uses. Same rules, same sentences (a failure has a log text that may name a disc file and a page text
// that never does), same order of work. No UIKit: the screen is ios_disc_import_view.h; the host tests are tests_disc_import.cpp.
// Hashing: CommonCrypto (SHA-1 for the packages' hash trees, SHA-256 for the pins). Storage: POSIX (O_EXCL|O_NOFOLLOW part file,
// fsync, mode 0400, rename, directory fsync). Where it writes: <Library/r>/private-trace/inputs/<sha256>.bin, the layout the iOS
// runtime already reads (the devicectl transfer's), staging in private-trace/disc-staging/disc-file.part.
// Reads only; never writes to an image. One copy at a time per process (begin()/end()).
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>

namespace recomp::apple::discimport {

// ======== errors (the Java exception kinds)
struct IoError : std::runtime_error {using std::runtime_error::runtime_error;};          // IOException: the source or the storage
struct Eof : IoError {using IoError::IoError;};                                           // EOFException: cut off
struct XdvdfsDamaged : IoError {using IoError::IoError;                                   // Xdvdfs.Damaged
    std::string what_() const {static const std::string p="damaged disc image: ";const std::string m=what();return m.rfind(p,0)==0?m.substr(p.size()):m;}};
struct StfsDamaged : IoError {using IoError::IoError;};                                   // Disc2Stfs.Damaged
struct StfsRefused : IoError {using IoError::IoError;};                                   // Disc2Stfs.Refused
struct WriteFailed : IoError {using IoError::IoError;};                                   // SetupDiscCopy.WriteFailed
struct Stopped : IoError {Stopped():IoError("stopped"){}};                                // Disc2Discs.Stopped

inline std::string reason(const std::exception& e) {const std::string m=e.what();return m.empty()?"error":m;}
inline std::string errnoText(const std::string& what,int err) {return what+": "+std::strerror(err);}

// ======== hashing
inline std::string hex(const unsigned char* d,size_t n) {
    static const char* digits="0123456789abcdef";std::string out(n*2,'0');
    for(size_t i=0;i<n;++i){out[i*2]=digits[(d[i]>>4)&15];out[i*2+1]=digits[d[i]&15];}
    return out;
}
struct Sha256 {
    CC_SHA256_CTX c;Sha256(){CC_SHA256_Init(&c);}
    void update(const uint8_t* d,size_t n){while(n){const CC_LONG k=CC_LONG(std::min<size_t>(n,1u<<30));CC_SHA256_Update(&c,d,k);d+=k;n-=k;}}
    std::string hexDigest(){unsigned char o[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(o,&c);return hex(o,sizeof o);}
};
inline void sha1(const uint8_t* d,size_t n,unsigned char out[20]) {CC_SHA1(d,CC_LONG(n),out);}
inline bool isSha256(const std::string& t) {
    if(t.size()!=64)return false;
    for(char c:t)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}

// ======== little / big endian
inline uint32_t le16(const uint8_t* d) {return uint32_t(d[0])|uint32_t(d[1])<<8;}
inline uint64_t le32(const uint8_t* d) {return uint64_t(d[0])|uint64_t(d[1])<<8|uint64_t(d[2])<<16|uint64_t(d[3])<<24;}
inline uint64_t be32(const uint8_t* d) {return uint64_t(d[0])<<24|uint64_t(d[1])<<16|uint64_t(d[2])<<8|uint64_t(d[3]);}
inline uint32_t le24(const uint8_t* d) {return uint32_t(d[0])|uint32_t(d[1])<<8|uint32_t(d[2])<<16;}

/** "2.1 GB": decimal gigabytes with one decimal; a need rounded up, a free amount down (VerifiedExtractor.gigabytes). */
inline std::string gigabytes(uint64_t bytes,bool roundUp) {
    const uint64_t tenths=roundUp?(bytes+99999999ull)/100000000ull:bytes/100000000ull;
    char b[48];std::snprintf(b,sizeof b,"%llu.%llu GB",(unsigned long long)(tenths/10),(unsigned long long)(tenths%10));return b;
}

// ======== sources (DiscSource, Disc2Window, a file)
/** Random-access, read-only bytes of a disc image (DiscSource). read: exactly n bytes at pos, or throws (Eof past the end). */
struct Source {
    virtual ~Source()=default;
    virtual uint64_t size() =0;
    virtual void read(uint64_t pos,uint8_t* buf,size_t n) =0;
};
/** A file the player picked (pread on a descriptor opened read-only; a security-scoped URL's path on iOS). Owns the descriptor. */
struct FileSource : Source {
    int fd=-1;uint64_t bytes=0;
    explicit FileSource(const std::string& path) {
        fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
        if(fd<0)throw IoError(errnoText("the file cannot be opened",errno));
        struct stat st{};if(::fstat(fd,&st)!=0){const int e=errno;::close(fd);fd=-1;throw IoError(errnoText("the file cannot be read",e));}
        if(!S_ISREG(st.st_mode)){::close(fd);fd=-1;throw IoError("it is not a regular file");}
        bytes=uint64_t(st.st_size);
#ifdef F_RDAHEAD
        ::fcntl(fd,F_RDAHEAD,1);
#endif
    }
    ~FileSource() override {if(fd>=0)::close(fd);}
    FileSource(const FileSource&)=delete;FileSource& operator=(const FileSource&)=delete;
    uint64_t size() override {return bytes;}
    void read(uint64_t pos,uint8_t* buf,size_t n) override {
        if(pos+n<pos || pos+n>bytes)throw Eof("read past the end of the file");
        size_t done=0;
        while(done<n) {
            const ssize_t r=::pread(fd,buf+done,n-done,off_t(pos+done));
            if(r<0){if(errno==EINTR)continue;throw IoError(errnoText("read failed",errno));}
            if(r==0)throw Eof("the file ended early");
            done+=size_t(r);
        }
    }
};
/** Bytes in memory (the tests). */
struct MemorySource : Source {
    std::vector<uint8_t> data;explicit MemorySource(std::vector<uint8_t> d):data(std::move(d)){}
    uint64_t size() override {return data.size();}
    void read(uint64_t pos,uint8_t* buf,size_t n) override {if(pos+n<pos || pos+n>data.size())throw Eof("read past the end");std::memcpy(buf,data.data()+pos,n);}
};
/** [offset, offset + length) of another source (Disc2Window). It does not own that source. */
struct Window : Source {
    Source& src;uint64_t offset,length;
    Window(Source& s,uint64_t off,uint64_t len):src(s),offset(off),length(len) {
        if(off+len<off)throw IoError("a window with a negative size or position");
        if(off+len>s.size())throw Eof("the image is cut off inside this file");
    }
    uint64_t size() override {return length;}
    void read(uint64_t pos,uint8_t* buf,size_t n) override {
        if(pos+n<pos)throw IoError("read outside the buffer or before the start");
        if(pos+n>length)throw Eof("read past the end of the file");
        src.read(offset+pos,buf,n);
    }
};

/** A byte stream: read up to n bytes, 0 at the end (InputStream). */
struct Stream {
    virtual ~Stream()=default;
    virtual size_t read(uint8_t* buf,size_t n) =0;
};
/** length bytes of source from offset on; a range past the end of the source is an Eof when read (Disc2Discs.range). */
struct RangeStream : Stream {
    Source& src;uint64_t at,end;
    RangeStream(Source& s,uint64_t off,uint64_t len):src(s),at(off),end(off+len){}
    size_t read(uint8_t* buf,size_t n) override {
        if(n==0 || at>=end)return 0;
        const size_t k=size_t(std::min<uint64_t>(n,end-at));
        if(at+k>src.size())throw Eof("the image is cut off inside this file");
        src.read(at,buf,k);at+=k;return k;
    }
};

// ======== Xdvdfs: the Xbox 360 disc's file system (Xdvdfs.java)
constexpr uint32_t SECTOR=2048;
constexpr uint64_t PARTITION_OFFSETS[]={0ull,0x0FD90000ull,0x02080000ull,0x18300000ull};
constexpr uint64_t MAX_DIRECTORY_BYTES=16u<<20;
constexpr int MAX_ENTRIES=100000,MAX_DEPTH=64;
constexpr uint32_t VOLUME_SECTOR=32;
inline const char* MEDIA_MAGIC="MICROSOFT*XBOX*MEDIA";
inline bool magicAt(const uint8_t* d) {return std::memcmp(d,MEDIA_MAGIC,20)==0;}
inline bool equalsIgnoreCase(const std::string& a,const std::string& b) {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(std::toupper((unsigned char)a[i])!=std::toupper((unsigned char)b[i]))return false;
    return true;
}
inline std::string upper(std::string s) {for(auto& c:s)c=char(std::toupper((unsigned char)c));return s;}

struct XEntry {std::string path;uint64_t offset=0,size=0;bool directory=false;};
class Xdvdfs {
public:
    /** Looks for the volume descriptor at sector 32 of each known partition offset; null if none. */
    static std::unique_ptr<Xdvdfs> open(Source& src) {
        const uint64_t size=src.size();std::vector<uint8_t> head(SECTOR);
        for(const uint64_t off:PARTITION_OFFSETS) {
            if(off+(VOLUME_SECTOR+1ull)*SECTOR>size)continue;
            src.read(off+uint64_t(VOLUME_SECTOR)*SECTOR,head.data(),SECTOR);
            if(magicAt(head.data()) && magicAt(head.data()+SECTOR-20))
                return std::unique_ptr<Xdvdfs>(new Xdvdfs(src,off,size,le32(head.data()+20),le32(head.data()+24)));
        }
        return nullptr;
    }
    uint64_t partitionOffset() const {return partition_;}
    /** Case-insensitive, '/' separated; null if absent (a directory is returned with directory = true). */
    std::unique_ptr<XEntry> find(const std::string& path) {
        uint64_t sector=rootSector_,size=rootSize_;std::unique_ptr<XEntry> hit;int depth=0;
        std::string p=path;std::replace(p.begin(),p.end(),'\\','/');
        size_t s=0;
        while(s<=p.size()) {
            size_t e=p.find('/',s);if(e==std::string::npos)e=p.size();
            const std::string part=p.substr(s,e-s);s=e+1;
            if(part.empty())continue;
            if(hit && !hit->directory)return nullptr;
            if(++depth>MAX_DEPTH)throw XdvdfsDamaged("damaged disc image: folders nested too deeply");
            std::unique_ptr<XEntry> next;const std::string prefix=hit?hit->path+"/":"";
            for(const auto& raw:readDirectory(sector,size))
                if(equalsIgnoreCase(raw.name,part)){next=std::make_unique<XEntry>(raw.entry(prefix,partition_));sector=raw.sector;size=raw.size;break;}
            if(!next)return nullptr;
            hit=std::move(next);
        }
        return hit;
    }
    /** All files, depth first, each directory's names in alphabetical order (upper case); refuses cycles and > 100000 entries. */
    std::vector<XEntry> list() {std::vector<XEntry> out;int seen=0;std::set<uint64_t> anc;walk(rootSector_,rootSize_,"",anc,out,seen,1);return out;}
private:
    struct Raw {std::string name;uint64_t sector,size;bool directory;
        XEntry entry(const std::string& prefix,uint64_t partition) const {return {prefix+name,partition+sector*SECTOR,size,directory};}};
    Source& src_;uint64_t partition_,imageSize_,rootSector_,rootSize_;
    Xdvdfs(Source& s,uint64_t part,uint64_t img,uint64_t rs,uint64_t rz):src_(s),partition_(part),imageSize_(img),rootSector_(rs),rootSize_(rz){}
    void walk(uint64_t sector,uint64_t size,const std::string& prefix,std::set<uint64_t>& anc,std::vector<XEntry>& out,int& seen,int depth) {
        if(depth>MAX_DEPTH)throw XdvdfsDamaged("damaged disc image: folders nested too deeply");
        if(!anc.insert(sector).second)throw XdvdfsDamaged("damaged disc image: a folder contains itself");
        auto raws=readDirectory(sector,size);
        std::stable_sort(raws.begin(),raws.end(),[](const Raw& a,const Raw& b){return upper(a.name)<upper(b.name);});
        for(const auto& raw:raws) {
            if(++seen>MAX_ENTRIES)throw XdvdfsDamaged("damaged disc image: more than 100000 entries");
            if(raw.directory)walk(raw.sector,raw.size,prefix+raw.name+"/",anc,out,seen,depth+1);
            else out.push_back(raw.entry(prefix,partition_));
        }
        anc.erase(sector);
    }
    std::vector<Raw> readDirectory(uint64_t sector,uint64_t size) {
        std::vector<Raw> out;if(size==0)return out;
        if(size>MAX_DIRECTORY_BYTES)throw XdvdfsDamaged("damaged disc image: a folder table is larger than 16 MiB");
        const uint64_t start=partition_+sector*SECTOR;
        if(start+size>imageSize_)throw Eof("the image is cut off inside a folder table");
        std::vector<uint8_t> data(size);src_.read(start,data.data(),data.size());
        std::set<uint32_t> visited;std::vector<uint32_t> todo{0};
        while(!todo.empty()) {
            const uint32_t at=todo.back();todo.pop_back();
            if(!visited.insert(at).second)throw XdvdfsDamaged("damaged disc image: a folder table loops");
            if(uint64_t(at)+14>data.size())throw XdvdfsDamaged("damaged disc image: a folder entry lies outside its table");
            const uint32_t left=le16(&data[at]),right=le16(&data[at+2]);const uint64_t first=le32(&data[at+4]),bytes=le32(&data[at+8]);
            if(left==0xFFFF && right==0xFFFF && first==0xFFFFFFFFull)continue; // padding
            const uint32_t attributes=data[at+12],nameLength=data[at+13];
            if(nameLength==0 || uint64_t(at)+14+nameLength>data.size())throw XdvdfsDamaged("damaged disc image: bad name in a folder table");
            const std::string name(reinterpret_cast<const char*>(&data[at+14]),nameLength); // (ISO-8859-1 bytes kept as they are)
            if(name.find('/')!=std::string::npos || name.find('\\')!=std::string::npos || name.find('\0')!=std::string::npos || name=="." || name=="..")
                throw XdvdfsDamaged("damaged disc image: unusable name in a folder table");
            out.push_back({name,first,bytes,(attributes&0x10)!=0});
            if(int(out.size())>MAX_ENTRIES)throw XdvdfsDamaged("damaged disc image: more than 100000 entries");
            if(left!=0 && left!=0xFFFF)todo.push_back(left*4);
            if(right!=0 && right!=0xFFFF)todo.push_back(right*4);
        }
        return out;
    }
};

// ======== the image probe (Disc2ImageProbe.java)
namespace probe {
enum class Kind {Trimmed,FullXgd2,FullXgd3,FullXgd1,CutOff,NotADiscImage,Unreadable};
struct Result {Kind kind;int64_t partitionOffset;std::string message,detail,what;bool packed=false;bool usable() const {return partitionOffset>=0;}};
inline const char* TEXT_CUT_OFF="This disc image is damaged: the disc's file system in it is incomplete or cut off.";
inline const char* TEXT_UNPACK="If a disc image is packed inside it, unpack it first.";
constexpr uint64_t MAX_ZIP_DIRECTORY=8u<<20;
inline Kind kindOf(uint64_t off) {
    if(off==0)return Kind::Trimmed;if(off==0x0FD90000ull)return Kind::FullXgd2;if(off==0x02080000ull)return Kind::FullXgd3;if(off==0x18300000ull)return Kind::FullXgd1;
    throw std::invalid_argument("not a known partition offset");
}
inline std::string describe(Kind k) {
    switch(k){case Kind::Trimmed:return "a trimmed disc image";case Kind::FullXgd2:return "a full disc image (XGD2 layout)";
              case Kind::FullXgd3:return "a full disc image (XGD3 layout)";case Kind::FullXgd1:return "a full disc image (XGD1 layout)";default:return "?";}
}
inline std::string hex8(uint64_t v) {char b[16];std::snprintf(b,sizeof b,"0x%08llX",(unsigned long long)v);return b;}
/** A ZIP whose central directory names AndroidManifest.xml at its top level (an Android app: nothing to unpack). */
inline bool androidApp(Source& img,uint64_t size) {
    const size_t tail=size_t(std::min<uint64_t>(size,65535+22));if(tail<22)return false;
    std::vector<uint8_t> end(tail);img.read(size-tail,end.data(),tail);
    for(int64_t i=int64_t(tail)-22;i>=0;--i) {
        if(le32(&end[size_t(i)])!=0x06054B50ull)continue;
        const uint64_t dirBytes=le32(&end[size_t(i)+12]),dirAt=le32(&end[size_t(i)+16]);
        if(dirBytes==0 || dirBytes>MAX_ZIP_DIRECTORY || dirAt+dirBytes>size)return false;
        std::vector<uint8_t> dir(dirBytes);img.read(dirAt,dir.data(),dir.size());
        size_t at=0;
        while(at+46<=dir.size() && le32(&dir[at])==0x02014B50ull) {
            const uint32_t name=le16(&dir[at+28]),extra=le16(&dir[at+30]),comment=le16(&dir[at+32]);
            if(at+46+name>dir.size())return false;
            if(std::string(reinterpret_cast<const char*>(&dir[at+46]),name)=="AndroidManifest.xml")return true;
            at+=46+name+extra+comment;
        }
        return false;
    }
    return false;
}
/** {log sentence end, player's clause or "", packed}. */
inline void sniff(Source& img,uint64_t size,std::string& log,std::string& what,bool& packed) {
    packed=false;what.clear();
    if(size<33ull*SECTOR){log="the file is only "+std::to_string(size)+" bytes, smaller than any disc image.";what="it is far too small";return;}
    uint8_t s[16];img.read(0,s,16);const std::string four(reinterpret_cast<const char*>(s),4);
    if(four=="PIRS" || four=="LIVE" || four=="CON "){log="it is an Xbox 360 content package, not a disc image.";what="it is an Xbox 360 content package (a download, an add-on or a save)";return;}
    if(four=="XEX2"){log="it is the game executable alone, not a disc image.";what="it is a single game program file";return;}
    if(four==std::string("PK\x03\x04",4)) {
        if(androidApp(img,size)){log="it is an Android app (a ZIP archive with AndroidManifest.xml).";what="it is an Android app";return;}
        log="it is a ZIP archive.";what="it is a ZIP archive";packed=true;return;
    }
    if(four=="Rar!"){log="it is a RAR archive.";what="it is a RAR archive";packed=true;return;}
    if(s[0]==0x37 && s[1]==0x7A && s[2]==0xBC && s[3]==0xAF){log="it is a 7-Zip archive.";what="it is a 7-Zip archive";packed=true;return;}
    if(size>=0x8006){uint8_t iso[5];img.read(0x8001,iso,5);
        if(std::memcmp(iso,"CD001",5)==0){log="it is an ordinary CD/DVD image without an Xbox 360 game partition at any known position (it may hold only the disc's video part).";
            what="it is an ordinary CD or DVD image (it may hold only the video part of a disc)";return;}}
    log="no Xbox 360 game partition at any of the known positions (0, 0x0FD90000, 0x02080000, 0x18300000).";
}
inline Result probeOrThrow(Source& img) {
    const uint64_t size=img.size();std::string cutOff;std::vector<uint8_t> head(SECTOR);
    for(const uint64_t off:PARTITION_OFFSETS) {
        if(off+33ull*SECTOR>size)continue;
        img.read(off+32ull*SECTOR,head.data(),SECTOR);
        if(!magicAt(head.data()) || !magicAt(head.data()+SECTOR-20))continue;
        const uint64_t rootSector=le32(head.data()+20),rootSize=le32(head.data()+24),rootStart=off+rootSector*SECTOR;
        if(rootSize==0 || rootSize>MAX_DIRECTORY_BYTES){if(cutOff.empty())cutOff="the disc file system at "+hex8(off)+" names an impossible root folder";continue;}
        if(rootStart+rootSize>size){if(cutOff.empty())cutOff="the disc file system at "+hex8(off)+" needs at least "+std::to_string(rootStart+rootSize)+" bytes but the file has "+std::to_string(size)+": the image is cut off";continue;}
        uint8_t first[14];img.read(rootStart,first,14);
        const uint32_t nameLength=first[13];const bool padding=first[0]==0xFF && first[1]==0xFF;
        if(padding || nameLength==0 || 14+nameLength>rootSize){if(cutOff.empty())cutOff="the root folder of the disc file system at "+hex8(off)+" is damaged";continue;}
        const Kind k=kindOf(off);
        return {k,int64_t(off),"Found "+describe(k)+", game partition at byte "+std::to_string(off)+".","","",false};
    }
    if(!cutOff.empty())return {Kind::CutOff,-1,TEXT_CUT_OFF,"Damaged image: "+cutOff+".","",false};
    std::string log,what;bool packed;sniff(img,size,log,what,packed);
    return {Kind::NotADiscImage,-1,"This file is not a disc image"+(what.empty()?std::string("."):": "+what+".")+(packed?std::string(" ")+TEXT_UNPACK:""),
            "Not a disc image this app reads: "+log,what,packed};
}
inline Result run(Source& img) {
    try {return probeOrThrow(img);}
    catch(const std::exception& e){return {Kind::Unreadable,-1,"Unreadable: "+reason(e)+".","","",false};}
}
} // namespace probe

// ======== Disc2Stfs: the install disc's PIRS STFS packages, every block checked against the hash tree
constexpr uint32_t BLOCK=4096,STFS_END=0xFFFFFF;constexpr uint64_t MAX_PACKAGE=8ull<<30;constexpr int MAX_STFS_ENTRIES=4096;
constexpr uint32_t HEADER_BYTES=0x971A;constexpr int64_t MAX_BLOCKS=170ll*170*170;constexpr size_t TABLE_CACHE=256,RUN_BLOCKS=64;
inline uint64_t dataOffset(uint64_t block,uint64_t base) {
    uint64_t physical=block;
    for(const uint64_t c:{170ull,28900ull,4913000ull}){physical+=(block+c)/c;if(block<c)break;}
    return base+physical*BLOCK;
}
inline uint64_t tableOffset(uint64_t block,int level,uint64_t base) {
    uint64_t physical;
    if(level==0)physical=block<170?0:(block/170)*171+block/28900+1+(block>=28900?1:0);
    else if(level==1)physical=block<28900?171:(block/28900)*29071+1;
    else if(level==2)physical=29071;
    else throw std::invalid_argument("unsupported hash level");
    return base+physical*BLOCK;
}
struct SEntry {int index;std::string path;uint64_t size;std::vector<uint32_t> blocks;bool directory;uint64_t created,modified;};
class Stfs {
public:
    /** Opens a package: header, hash tree as far as the directory and the chains reach, and the directory. Never writes. */
    static std::unique_ptr<Stfs> open(Source& src,uint64_t titleId) {
        const uint64_t size=src.size();
        if(size<HEADER_BYTES || size>MAX_PACKAGE)throw StfsRefused("not a package this app reads: "+std::to_string(size)+" bytes is outside 38,682 bytes to 8 GiB");
        std::vector<uint8_t> h(HEADER_BYTES);src.read(0,h.data(),HEADER_BYTES);
        if(std::memcmp(h.data(),"PIRS",4)!=0)throw StfsRefused("not a PIRS package");
        const uint64_t version=be32(&h[0x348]);
        if(be32(&h[0x344])!=2 || (version!=1 && version!=2))throw StfsRefused("a package type or version this app does not read");
        const uint64_t title=be32(&h[0x360]);
        if(title!=titleId){char b[16];std::snprintf(b,sizeof b,"%08llX",(unsigned long long)title);throw StfsRefused(std::string("a package of another game (title ")+b+")");}
        const uint64_t headerSize=be32(&h[0x340]);
        if(headerSize<HEADER_BYTES || headerSize>std::min<uint64_t>(size,1u<<20))throw StfsDamaged("damaged package: its header size is impossible");
        const uint64_t base=(headerSize+BLOCK-1)&~uint64_t(BLOCK-1);
        const size_t v=0x379;
        bool zero=true;for(size_t i=0x39D;i<0x3AD;++i)zero=zero && h[i]==0;
        if(h[v]!=0x24 || h[v+1]!=0 || h[v+2]!=1 || !zero)throw StfsRefused("only single-file read-only packages of format version zero are read");
        const uint64_t total=be32(&h[v+28]),free_=be32(&h[v+32]);
        const uint32_t directoryCount=uint32_t(h[v+3])|uint32_t(h[v+4])<<8,firstDirectory=le24(&h[v+5]);
        if(total<1 || total>std::min<uint64_t>(MAX_BLOCKS,size/BLOCK) || free_!=0 || directoryCount<1 || directoryCount>MAX_STFS_ENTRIES/64
           || dataOffset(total-1,base)+BLOCK>size)
            throw StfsDamaged("damaged package: its block counts do not fit the file (it may be cut off)");
        // (the display name: valid UTF-16BE up to its first NUL - checked as the Java decoder does)
        for(size_t i=0x411;i+1<0x411+256;i+=2) {
            const uint32_t u=uint32_t(h[i])<<8|h[i+1];
            if(u==0)break;
            if(u>=0xD800 && u<=0xDBFF){const uint32_t lo=i+3<0x411+256?(uint32_t(h[i+2])<<8|h[i+3]):0;if(lo<0xDC00 || lo>0xDFFF)throw StfsDamaged("damaged package: its display name is not valid text");i+=2;}
            else if(u>=0xDC00 && u<=0xDFFF)throw StfsDamaged("damaged package: its display name is not valid text");
        }
        std::unique_ptr<Stfs> p(new Stfs(src,size,base,uint32_t(total),directoryCount,firstDirectory));
        std::memcpy(p->rootHash_,&h[v+8],20);
        p->entries_=p->directory();
        p->checkUnchanged();
        return p;
    }
    const std::vector<SEntry>& entries() const {return entries_;}
    /** The entry's bytes; every block's SHA-1 is checked before any of its bytes are handed out (StfsDamaged on a mismatch). */
    std::unique_ptr<Stream> openEntry(const SEntry& e) {
        if(e.directory)throw IoError("only a file of this package can be read");
        return std::make_unique<EntryStream>(*this,e);
    }
private:
    Source& src_;uint64_t size_,base_;uint32_t total_,directoryCount_,firstDirectory_;int rootLevel_;
    unsigned char rootHash_[20]{};std::vector<bool> claimed_;std::map<uint64_t,std::vector<uint8_t>> tables_;std::vector<SEntry> entries_;
    Stfs(Source& s,uint64_t size,uint64_t base,uint32_t total,uint32_t dc,uint32_t fd)
        :src_(s),size_(size),base_(base),total_(total),directoryCount_(dc),firstDirectory_(fd),
         rootLevel_(total<=170?0:total<=28900?1:2),claimed_(total,false){}
    std::vector<uint8_t> readAt(uint64_t off,size_t n) {
        if(off+n>size_)throw StfsDamaged("damaged package: a read leaves the package");
        std::vector<uint8_t> d(n);src_.read(off,d.data(),n);return d;
    }
    const std::vector<uint8_t>& table(uint32_t block,int level) {
        const uint64_t off=tableOffset(block,level,base_);
        if(auto it=tables_.find(off);it!=tables_.end())return it->second;
        unsigned char expected[20];
        if(level==rootLevel_)std::memcpy(expected,rootHash_,20);
        else {const auto& parent=table(block,level+1);const size_t slot=size_t((block/(level==0?170ull:28900ull))%170);std::memcpy(expected,&parent[slot*24],20);}
        auto data=readAt(off,BLOCK);unsigned char got[20];sha1(data.data(),BLOCK,got);
        if(std::memcmp(got,expected,20)!=0)throw StfsDamaged("damaged package: a hash table does not match its hash (level "+std::to_string(level)+", block "+std::to_string(block)+")");
        if(tables_.size()>=TABLE_CACHE)tables_.clear();
        return tables_.emplace(off,std::move(data)).first->second;
    }
    /** The block's next block; its expected SHA-1 into hash. */
    uint32_t record(uint32_t block,unsigned char hash[20]) {
        if(block>=total_)throw StfsDamaged("damaged package: a block number lies outside the package");
        const auto& t=table(block,0);const size_t at=block%170*24;const uint64_t info=be32(&t[at+20]);
        if(info>>30!=2)throw StfsDamaged("damaged package: a file uses a block that is not allocated");
        std::memcpy(hash,&t[at],20);return uint32_t(info&STFS_END);
    }
    std::vector<uint32_t> chain(uint32_t start,uint32_t count) {
        if(count>total_)throw StfsDamaged("damaged package: a chain is longer than the package");
        std::vector<uint32_t> out(count);unsigned char hash[20];uint32_t cur=start;
        for(uint32_t i=0;i<count;++i) {
            const uint32_t following=record(cur,hash);
            if(claimed_[cur])throw StfsDamaged("damaged package: two files share a block, or a chain loops");
            claimed_[cur]=true;out[i]=cur;cur=following;
        }
        if(count>0 && cur!=STFS_END)throw StfsDamaged("damaged package: a chain is longer than its record says");
        return out;
    }
    std::vector<uint8_t> block(uint32_t n) {
        unsigned char expected[20];record(n,expected);
        auto d=readAt(dataOffset(n,base_),BLOCK);unsigned char got[20];sha1(d.data(),BLOCK,got);
        if(std::memcmp(got,expected,20)!=0)throw StfsDamaged("damaged package: the data of block "+std::to_string(n)+" does not match its hash");
        return d;
    }
    static std::string component(const uint8_t* d,int length) {
        std::string name;
        for(int i=0;i<length;++i){const int c=d[i];if(c<32 || c>=128 || std::strchr("/\\:*?\"<>|",c))throw StfsDamaged("damaged package: unsafe name in its directory");name+=char(c);}
        if(name=="." || name==".." || name.back()=='.' || name.back()==' ')throw StfsDamaged("damaged package: unsafe name in its directory");
        return name;
    }
    std::vector<SEntry> directory() {
        const auto ch=chain(firstDirectory_,directoryCount_);
        std::vector<SEntry> out;std::map<int,size_t> byIndex;std::set<std::string> seen;
        for(size_t b=0;b<ch.size();++b) {
            const auto data=block(ch[b]);
            for(int slot=0;slot<64;++slot) {
                const int index=int(b)*64+slot;const size_t r=size_t(slot)*64;
                bool zero=true;for(size_t i=r;i<r+64;++i)zero=zero && data[i]==0;
                if(zero)continue;
                const int length=data[r+40]&63;const bool isDir=(data[r+40]&0x80)!=0;
                bool tailZero=true;for(size_t i=r+size_t(length);i<r+40;++i)tailZero=tailZero && data[i]==0;
                if(length<1 || length>40 || !tailZero)throw StfsDamaged("damaged package: a name in its directory is malformed");
                std::string name=component(&data[r],length);
                const int parent=int(data[r+50])<<8|data[r+51];
                if(parent!=0xFFFF) {
                    auto it=byIndex.find(parent);
                    if(it==byIndex.end() || !out[it->second].directory)throw StfsDamaged("damaged package: a directory record names a parent that is not a folder before it");
                    name=out[it->second].path+"/"+name;
                }
                std::string lower=name;for(auto& c:lower)c=char(std::tolower((unsigned char)c));
                if(name.size()>1024 || std::count(name.begin(),name.end(),'/')>32 || !seen.insert(lower).second)throw StfsDamaged("damaged package: a path is repeated or nested too deeply");
                const uint32_t valid=le24(&data[r+41]),allocated=le24(&data[r+44]),start=le24(&data[r+47]);const uint64_t fileSize=be32(&data[r+52]);
                if(allocated!=valid || uint64_t(allocated)!=(fileSize+BLOCK-1)/BLOCK || (isDir && (allocated!=0 || fileSize!=0)))
                    throw StfsDamaged("damaged package: a file's size and its block count disagree");
                auto blocks=chain(start,allocated);
                if(data[r+40]&0x40)for(size_t i=0;i<blocks.size();++i)if(blocks[i]!=start+i)throw StfsDamaged("damaged package: a file marked contiguous is not");
                byIndex[index]=out.size();
                out.push_back({index,name,fileSize,std::move(blocks),isDir,be32(&data[r+56]),be32(&data[r+60])});
            }
        }
        return out;
    }
    void checkUnchanged() {if(src_.size()!=size_)throw IoError("the package changed while it was read");}
    struct EntryStream : Stream {
        Stfs& p;const SEntry& e;std::vector<uint8_t> run=std::vector<uint8_t>(RUN_BLOCKS*BLOCK);size_t next=0;uint64_t left;size_t at=0,end=0;
        EntryStream(Stfs& pk,const SEntry& en):p(pk),e(en),left(en.size){}
        size_t read(uint8_t* buf,size_t n) override {
            if(n==0)return 0;
            if(at==end && !fill())return 0;
            const size_t k=std::min(n,end-at);std::memcpy(buf,&run[at],k);at+=k;return k;
        }
        bool fill() {
            if(left==0)return false;
            const uint32_t first=e.blocks[next];const uint64_t physical=dataOffset(first,p.base_);size_t count=1;
            while(next+count<e.blocks.size() && count<RUN_BLOCKS && dataOffset(e.blocks[next+count],p.base_)==physical+count*BLOCK)++count;
            if(physical+count*BLOCK>p.size_)throw StfsDamaged("damaged package: a read leaves the package");
            p.src_.read(physical,run.data(),count*BLOCK);
            unsigned char expected[20],got[20];
            for(size_t i=0;i<count;++i) {
                p.record(e.blocks[next+i],expected);sha1(&run[i*BLOCK],BLOCK,got);
                if(std::memcmp(got,expected,20)!=0)throw StfsDamaged("damaged package: the data of block "+std::to_string(e.blocks[next+i])+" of "+e.path+" does not match its hash");
            }
            next+=count;at=0;end=size_t(std::min<uint64_t>(left,uint64_t(count)*BLOCK));left-=end;
            if(left==0)p.checkUnchanged();
            return true;
        }
    };
};

// ======== Disc2Discs: which disc an image is, and what it offers (Disc2Discs.java)
constexpr uint64_t INSTALL_CONTENT_TYPE=2;
inline const char* SYSTEM_UPDATE_FOLDER="$SystemUpdate/";
constexpr uint64_t MAX_EXECUTABLE_BYTES=256ull<<20;constexpr size_t BUFFER_BYTES=1u<<20;
inline const char* NEXT_NEW_COPY="Copy the image to this device again, or make a new copy of the disc.";
inline const char* NEXT_RECONNECT="Check that the storage it is on is still connected, then choose it again.";
inline const char* NEXT_CHOOSE_IMAGE="Choose the image file of the disc, usually an .iso file.";
// The import page's help line (owner 2026-11-05: deleting the images is the player's step once both discs are in; the app never removes
// or changes an image). installIn / playIn: the files of each disc in place (of 8 / 10); while an import runs the read-only line stays.
inline const char* TEXT_BOTH_DISCS_IN_PLACE="Both discs are in place. The game's files from the discs are complete.";
inline const char* HELP_READ_ONLY="The images are read only; nothing on them is changed. An image on a USB drive works through Choose A Disc Image. About 16.4 GB is needed on this device.";
#if defined(RECOMP_DUO_EDITION) // ios-duo (2026-11-05): the Duo is an iPhone; its Files folder carries the app's display name (duo_build.py)
inline const char* HELP_IMAGES_NOT_NEEDED="You can now delete the two disc images to free about 17.5 GB; the game no longer needs them. Images copied into this app are in the Files app under On My iPhone, in the FivePocket Duo folder.";
#else
inline const char* HELP_IMAGES_NOT_NEEDED="You can now delete the two disc images to free about 17.5 GB; the game no longer needs them. Images copied into this app are in the Files app under On My iPad, in this app's folder.";
#endif
inline const char* pageHelp(int installIn,int playIn,bool busy) {return !busy && installIn==8 && playIn==10?HELP_IMAGES_NOT_NEEDED:HELP_READ_ONLY;}
enum class Kind {Play,Install,OtherGame,NotADisc,Damaged,Unreadable};
struct Game {std::string label,executableName,executableSha256;uint64_t titleId;};
/** One file a disc offers: name (log only), size, block-checked (install disc), and how to open its bytes. */
struct Candidate {std::string name;uint64_t bytes;bool blockChecked;std::function<std::unique_ptr<Stream>()> open;};
struct Disc {
    Kind kind;std::string message,named,next;int64_t partitionOffset=-1;
    std::vector<Candidate> candidates;std::vector<std::string> packages,ignored;
    // what the candidates read through (kept alive with the Disc): the image, the partition window, the opened packages
    std::shared_ptr<Source> image;std::shared_ptr<Window> partition;std::vector<std::shared_ptr<Window>> windows;std::vector<std::shared_ptr<Stfs>> opened;
    bool usable() const {return kind==Kind::Play || kind==Kind::Install;}
    std::string logged() const {return named.empty()?message:message+" ["+named+"]";}
};
inline std::string nextSupported(const Game& g) {
    return "This version works with the two discs of "+g.label+" only: choose the image of its play disc (Disc 2) or of its"
           " install disc (Disc 1). If this image was made from one of them, make a new copy of that disc: this copy is damaged.";
}
inline std::string sizeClause(uint64_t bytes) {return bytes<100000000ull?"it is smaller than 0.1 GB":"it is only "+gigabytes(bytes,false);}
inline Disc refused(Kind k,std::string message,std::string named,std::string next) {Disc d;d.kind=k;d.message=std::move(message);d.named=std::move(named);d.next=std::move(next);return d;}
inline Disc notSupported(const Game& g,std::string message,std::string named,int64_t part,std::vector<std::string> ignored) {
    Disc d=refused(Kind::OtherGame,std::move(message),std::move(named),nextSupported(g));d.partitionOffset=part;d.ignored=std::move(ignored);return d;
}
/** content/SIXTEEN HEX/TITLE/00000002/NAME: the folder a game disc keeps this title's install content in. */
inline bool inInstallFolder(const std::string& path,uint64_t titleId) {
    std::vector<std::string> parts;size_t s=0;
    for(;;){const size_t e=path.find('/',s);parts.push_back(path.substr(s,e==std::string::npos?std::string::npos:e-s));if(e==std::string::npos)break;s=e+1;}
    if(parts.size()!=5 || !equalsIgnoreCase(parts[0],"content") || parts[1].size()!=16)return false;
    for(char c:parts[1])if(!std::isxdigit((unsigned char)c))return false;
    char t[16],c[16];std::snprintf(t,sizeof t,"%08llX",(unsigned long long)titleId);std::snprintf(c,sizeof c,"%08llX",(unsigned long long)INSTALL_CONTENT_TYPE);
    return equalsIgnoreCase(parts[2],t) && parts[3]==c;
}

/** Told after every part read how many bytes of this file are done; false stops the read (Stopped). */
using Watch=std::function<bool(uint64_t)>;
/** An output for the bytes read (the part file); throws WriteFailed on a failure of the device's storage. */
using Sink=std::function<void(const uint8_t*,size_t)>;
/** Reads exactly bytes from in, optionally writing them, and returns the SHA-256 (fewer = Eof, more = IoError). */
inline std::string sha256Stream(Stream& in,uint64_t bytes,const Sink* out,const Watch* watch) {
    Sha256 sha;std::vector<uint8_t> buf(size_t(std::max<uint64_t>(1,std::min<uint64_t>(BUFFER_BYTES,bytes))));uint64_t done=0;
    while(done<bytes) {
        const size_t n=in.read(buf.data(),size_t(std::min<uint64_t>(buf.size(),bytes-done)));
        if(n==0)throw Eof("the file ends after "+std::to_string(done)+" of "+std::to_string(bytes)+" bytes");
        sha.update(buf.data(),n);if(out)(*out)(buf.data(),n);done+=n;
        if(watch && !(*watch)(done))throw Stopped();
    }
    uint8_t one;if(in.read(&one,1)>0)throw IoError("the file is longer than "+std::to_string(bytes)+" bytes");
    return sha.hexDigest();
}
inline std::string sha256Candidate(const Candidate& c,const Sink* out,const Watch* watch) {auto in=c.open();return sha256Stream(*in,c.bytes,out,watch);}

/** Never throws; never writes. Which disc image is, with the sentences, and its candidate files. image: kept by the Disc. */
inline Disc openOrThrow(const std::shared_ptr<Source>& image,const Game& game) {
    const auto pr=probe::run(*image);
    if(!pr.usable()) {
        if(pr.kind==probe::Kind::CutOff)return refused(Kind::Damaged,pr.message,pr.detail,NEXT_NEW_COPY);
        if(pr.kind==probe::Kind::Unreadable)return refused(Kind::Unreadable,pr.message,"",NEXT_RECONNECT);
        return refused(Kind::NotADisc,"This file is not a disc image of "+game.label+(pr.what.empty()?std::string("."):": "+pr.what+".")+(pr.packed?std::string(" ")+probe::TEXT_UNPACK:""),
                       pr.detail,NEXT_CHOOSE_IMAGE);
    }
    const uint64_t imageSize=image->size();
    auto partition=std::make_shared<Window>(*image,uint64_t(pr.partitionOffset),imageSize-uint64_t(pr.partitionOffset));
    auto fs=Xdvdfs::open(*partition);
    if(!fs || fs->partitionOffset()!=0)return refused(Kind::NotADisc,"This file is not an Xbox 360 disc image that this app can read.","",NEXT_CHOOSE_IMAGE);
    const auto files=fs->list();
    for(const auto& e:files)
        if(e.offset+e.size>partition->size())
            return refused(Kind::Damaged,"This disc image is incomplete: it ends before the disc's data does ("+sizeClause(imageSize)+").",
                           e.path+" goes on to byte "+std::to_string(uint64_t(pr.partitionOffset)+e.offset+e.size)+", the image has "+std::to_string(imageSize)+" bytes",NEXT_NEW_COPY);
    Disc out;out.image=image;out.partition=partition;out.partitionOffset=pr.partitionOffset;
    std::vector<uint8_t> head(0x364);
    for(const auto& e:files) {
        std::string why,plain,note;bool isInstall=false;
        if(e.size<HEADER_BYTES)why="it is too small to be a content package";
        else {
            partition->read(e.offset,head.data(),head.size());
            const uint64_t type=be32(&head[0x344]),title=be32(&head[0x360]);char tt[16];std::snprintf(tt,sizeof tt,"%08llX",(unsigned long long)title);
            if(std::memcmp(head.data(),"PIRS",4)!=0)why="it does not begin as a content package does";
            else if(type!=INSTALL_CONTENT_TYPE){why="its header names content type "+std::to_string(type)+", not install content";plain="it is a package of another kind, not install content";
                note=e.path+" (a package of content type "+std::to_string(type)+", not install content)";}
            else if(title!=game.titleId){why=std::string("its header names another title, ")+tt;plain="it belongs to another game";note=e.path+" (install content of another title, "+tt+")";}
            else isInstall=true;
        }
        if(!isInstall) {
            if(inInstallFolder(e.path,game.titleId))
                return notSupported(game,"This disc image cannot be used: a file in the game's install-content folder is not an install package of the game ("+(plain.empty()?why:plain)+").",
                                    plain.empty()?e.path:e.path+" ("+why+")",pr.partitionOffset,out.ignored);
            if(!note.empty())out.ignored.push_back(note);
            continue;
        }
        const std::string name=e.path.substr(e.path.rfind('/')==std::string::npos?0:e.path.rfind('/')+1);
        auto window=std::make_shared<Window>(*partition,e.offset,e.size);
        std::shared_ptr<Stfs> opened;
        try {opened=std::shared_ptr<Stfs>(Stfs::open(*window,game.titleId).release());}
        catch(const StfsDamaged& d){return refused(Kind::Damaged,"This disc image is damaged: one of its content packages does not pass its own checks.",e.path+": "+d.what(),NEXT_NEW_COPY);}
        catch(const StfsRefused& r){return notSupported(game,"This disc image cannot be used: one of its content packages is in a form this version does not read.",
                                                         e.path+": "+r.what(),pr.partitionOffset,out.ignored);}
        out.packages.push_back(e.path);out.windows.push_back(window);out.opened.push_back(opened);
        for(const auto& entry:opened->entries()) {
            if(entry.directory)continue;
            Stfs* pk=opened.get();const SEntry* en=&entry;
            out.candidates.push_back({name+"/"+entry.path,entry.size,true,[pk,en]{return pk->openEntry(*en);}});
        }
    }
    if(!out.packages.empty()) {
        out.kind=Kind::Install;const size_t p=out.packages.size(),c=out.candidates.size();
        out.message="Recognised: "+game.label+", the install disc ("+std::to_string(p)+(p==1?" content package, ":" content packages, ")+std::to_string(c)+(c==1?" file).":" files).");
        return out;
    }
    const auto exe=fs->find(game.executableName);
    if(!exe || exe->directory)return notSupported(game,"This disc image holds no game program, so it is not a disc of "+game.label+".",game.executableName,pr.partitionOffset,out.ignored);
    if(exe->size>MAX_EXECUTABLE_BYTES)return notSupported(game,"This is not a disc of "+game.label+" (its game program is larger than the supported one).","",pr.partitionOffset,out.ignored);
    {RangeStream rs(*partition,exe->offset,exe->size);
     if(sha256Stream(rs,exe->size,nullptr,nullptr)!=game.executableSha256)
         return notSupported(game,"This is not the play disc of "+game.label+", or its copy is damaged: its game program is not the supported one.","",pr.partitionOffset,out.ignored);}
    for(const auto& e:files) {
        if(e.path.size()>=std::strlen(SYSTEM_UPDATE_FOLDER) && equalsIgnoreCase(e.path.substr(0,std::strlen(SYSTEM_UPDATE_FOLDER)),SYSTEM_UPDATE_FOLDER))continue;
        Window* part=partition.get();const uint64_t off=e.offset,len=e.size;
        out.candidates.push_back({e.path,e.size,false,[part,off,len]{return std::unique_ptr<Stream>(new RangeStream(*part,off,len));}});
    }
    out.kind=Kind::Play;const size_t c=out.candidates.size();
    out.message="Recognised: "+game.label+", the play disc ("+std::to_string(c)+(c==1?" file).":" files).");
    return out;
}
inline Disc open(const std::shared_ptr<Source>& image,const Game& game) {
    try {return openOrThrow(image,game);}
    catch(const Eof&) {
        std::string only,bytes;try{only=" ("+sizeClause(image->size())+")";bytes="image bytes="+std::to_string(image->size());}catch(...){}
        return refused(Kind::Damaged,"This disc image is incomplete: it ends before the disc's data does"+only+".",bytes,NEXT_NEW_COPY);
    }
    catch(const XdvdfsDamaged& e){return refused(Kind::Damaged,"This disc image is damaged ("+e.what_()+").","",NEXT_NEW_COPY);}
    catch(const std::exception& e){return refused(Kind::Unreadable,"The disc image could not be read ("+reason(e)+").","",NEXT_RECONNECT);}
}

// ---- the copy plan
struct Pin {std::string sha256;uint64_t bytes;};
enum class Verdict {Wanted,AlreadyInstalled,NotPinned};
inline std::string pinKey(const Pin& p) {return p.sha256+"/"+std::to_string(p.bytes);}
struct Plan {
    std::vector<const Candidate*> read;uint64_t bytes=0;std::vector<Pin> wanted,noCandidate;std::vector<uint64_t> shortSizes;
    std::vector<Pin> pinned;std::set<std::string> installed;
    const Pin* pin(uint64_t b,const std::string& sha) const {for(const auto& p:pinned)if(p.bytes==b && p.sha256==sha)return &p;return nullptr;}
    Verdict judge(uint64_t b,const std::string& sha) const {const Pin* p=pin(b,sha);if(!p)return Verdict::NotPinned;return installed.count(p->sha256)?Verdict::AlreadyInstalled:Verdict::Wanted;}
};
/** Which candidates to read: every candidate of a size some not-installed pin has (its content is known only after reading). */
inline Plan plan(const std::vector<Candidate>& candidates,const std::vector<Pin>& pinned,const std::set<std::string>& installed) {
    Plan p;p.installed=installed;std::set<std::string> seen;std::set<uint64_t> wantedSizes,candidateSizes;
    for(const auto& x:pinned) {
        if(!seen.insert(pinKey(x)).second)continue;
        p.pinned.push_back(x);
        if(!installed.count(x.sha256)){p.wanted.push_back(x);wantedSizes.insert(x.bytes);}
    }
    std::map<uint64_t,int> unmatched;
    for(const auto& c:candidates) {
        candidateSizes.insert(c.bytes);++unmatched[c.bytes];
        if(!wantedSizes.count(c.bytes))continue;
        p.read.push_back(&c);p.bytes+=c.bytes;
    }
    for(const auto& w:p.wanted) {
        if(!candidateSizes.count(w.bytes))p.noCandidate.push_back(w);
        auto it=unmatched.find(w.bytes);
        if(it==unmatched.end() || it->second==0)p.shortSizes.push_back(w.bytes);else --it->second;
    }
    return p;
}
/** A regular file under exactly this name, not followed through a link; its size into bytes. */
inline bool regularFile(const std::string& path,uint64_t& bytes) {
    struct stat st{};if(::lstat(path.c_str(),&st)!=0 || !S_ISREG(st.st_mode))return false;bytes=uint64_t(st.st_size);return true;
}
/** The pins in place in inputs: a regular file <sha256>.bin of exactly the pinned size (content taken on trust: named only after it was verified). */
inline std::set<std::string> installed(const std::string& inputsDir,const std::vector<Pin>& pins) {
    std::set<std::string> out;
    for(const auto& p:pins){uint64_t b=0;if(regularFile(inputsDir+"/"+p.sha256+".bin",b) && b==p.bytes)out.insert(p.sha256);}
    return out;
}
inline std::vector<Pin> notIn(const std::vector<Pin>& pins,const std::set<std::string>& inst) {
    std::vector<Pin> out;std::set<std::string> seen;
    for(const auto& p:pins)if(seen.insert(pinKey(p)).second && !inst.count(p.sha256))out.push_back(p);
    return out;
}
inline std::vector<Pin> missing(const std::string& inputsDir,const std::vector<Pin>& pins) {return notIn(pins,installed(inputsDir,pins));}

// ======== SetupCatalog: the 18 files the game needs from the two discs (names, sizes and SHA-256 only)
namespace catalog {
inline const char* PLAY="play";inline const char* INSTALL="install";
struct Item {const char* disc;const char* path;uint64_t bytes;const char* sha256;};
inline const char* LABEL="GTA V for Xbox 360 (disc release)";
inline const Item ITEMS[]={
    {PLAY,"default.xex",26628096ull,"f0ca93fbe6b3dfbf50c026e0d77a71e8294b13a5a775af1b6a64f07b0fc10a3d"},
    {PLAY,"common.rpf",12916736ull,"08e44da0c420f1c6686ea09faed83d085351cd56b973e1945554012521bcdeea"},
    {PLAY,"xbox360a.rpf",711852032ull,"5c7a633870485a4e79738fa6169e5a7937b39ba4474fb6f51a994c8dc6b8e525"},
    {PLAY,"xbox360b.rpf",1431193600ull,"9d15357ffb79a815020f2bdb66b0faee8ec0873a6499c927569f236bdda41cb1"},
    {PLAY,"common/data/installpartitions.meta",1028ull,"159902888e5406ce4fff59831a9ecf7b4d0816080f65ddd7789d2038091ffd16"},
    {PLAY,"audio_rel.rpf",3108864ull,"04f4fd93791723c5205b814290003c86fd23a99910edaeb58a338fff9af3cce9"},
    {PLAY,"sfx/audio_cs.rpf",666355712ull,"14558590d94ce5af23b416a79e298d47e335878a3d932639a9f4ff3d9175f7c7"},
    {PLAY,"sfx/audio_misc.rpf",2762117120ull,"9d8b825c57029213340730514f92355f71208c8451fb669e1e4a702bc27e36a3"},
    {PLAY,"sfx/audio_music.rpf",1141999616ull,"c541e611f63a34d71ea409c9882770901161462985c30acebaa828ea15626739"},
    {PLAY,"sfx/audio_radio.rpf",1390331904ull,"55e9a283545f813a1ec0896d757c716c524e0261aa72da27ad368c2b0726b8a9"},
    {INSTALL,"545408A700000000/part0.rpf",2133954560ull,"ae1c120717f93e397f7561798bc9e8e42ef9fa367e250bf12c774cd86ddb7d88"},
    {INSTALL,"545408A700000000/part0.timestamp",14ull,"96bc0b2719da62e933476c095a16ad54157f38753da465209c58a3802bfe52bd"},
    {INSTALL,"545408A700000001/part1.rpf",2134566912ull,"78c21d9a783186e0ae4a4828d9b72fa27f5f7938177079e73d1f039e4f6dbfd8"},
    {INSTALL,"545408A700000001/part1.timestamp",14ull,"20b768dd4d6a085fdc8002ebab2467df7f7ac6c6c4b5e16b0fa3337a9e381db1"},
    {INSTALL,"545408A700000002/part2.rpf",2134116352ull,"ad9b4679a03508dfeaf729478fb43de867d9f8756f4d4b56e100f20b4af9926e"},
    {INSTALL,"545408A700000002/part2.timestamp",14ull,"a27533bb1a9f15d9e273f24cef04b68484da96f9bf9d5e4b0b0ddd6f1989761c"},
    {INSTALL,"545408A700000003/part3.rpf",1806764032ull,"1566241deb22e940e0cf7a0408af64f725f9ab82ea008137587f31403bbafb38"},
    {INSTALL,"545408A700000003/part3.timestamp",14ull,"ab18d5eaa3bd6d85fd563cabda550f11c5c6b59842ec67336a9b6ecf4fffe874"}};
inline Game game() {return {LABEL,"default.xex","f0ca93fbe6b3dfbf50c026e0d77a71e8294b13a5a775af1b6a64f07b0fc10a3d",0x545408A7ull};}
/** The pins of one disc ("play" / "install"), or of both (nullptr). */
inline std::vector<Pin> pins(const char* disc=nullptr) {
    std::vector<Pin> out;for(const auto& i:ITEMS)if(!disc || std::strcmp(disc,i.disc)==0)out.push_back({i.sha256,i.bytes});return out;
}
} // namespace catalog

// ======== SetupDiscCopy with the VerifiedExtractor safeguards
enum class Problem {None,NoSpace,DiscCopyBad,SourceUnreadable,Storage,Busy,Other};
inline const char* BUSY_MESSAGE="The last copy or import has not stopped yet.";
inline std::atomic<bool>& running() {static std::atomic<bool> r{false};return r;}
/** One copy at a time in this process: two runs would fight over the same staging file. */
inline bool begin() {bool f=false;return running().compare_exchange_strong(f,true);}
inline void end() {running().store(false);}
inline const char* STAGING="disc-staging";inline const char* PART="disc-file.part";inline const char* INPUTS="inputs";
constexpr uint64_t RESERVE_BYTES=1ull<<30;

/** Free bytes at an existing directory (statfs; f_bavail: what a non-root writer gets). Tests inject their own. */
using Space=std::function<uint64_t(const std::string&)>;
inline uint64_t diskSpace(const std::string& dir) {struct statfs s{};if(::statfs(dir.c_str(),&s)!=0)return 0;return uint64_t(s.f_bavail)*uint64_t(s.f_bsize);}
inline std::string existingAncestor(std::string p) {
    struct stat st{};
    while(!p.empty()){if(::stat(p.c_str(),&st)==0 && S_ISDIR(st.st_mode))return p;const size_t s=p.rfind('/');if(s==std::string::npos || s==0)return s==0?"/":".";p=p.substr(0,s);}
    return ".";
}
/** A real directory, made with its missing parents; a link or a file in its place is an error (never followed or replaced). */
inline void makeDirectory(const std::string& dir) {
    struct stat st{};
    if(::lstat(dir.c_str(),&st)==0){if(S_ISDIR(st.st_mode))return;throw IoError("cannot create "+dir.substr(dir.rfind('/')+1)+": something else is in its place");}
    const size_t s=dir.rfind('/');if(s!=std::string::npos && s>0)makeDirectory(dir.substr(0,s));
    if(::mkdir(dir.c_str(),0700)!=0){if(errno==EEXIST)throw IoError("cannot create "+dir.substr(dir.rfind('/')+1)+": something else is in its place");throw IoError(errnoText("cannot create a folder",errno));}
}
inline void syncDirectory(const std::string& dir) {const int fd=::open(dir.c_str(),O_RDONLY|O_DIRECTORY);if(fd>=0){::fsync(fd);::close(fd);}}

struct FileResult {int index,files;std::string name;uint64_t bytes;std::string sha256;Verdict verdict;bool kept;};
struct Outcome {
    bool complete=false,stopped=false;Problem problem=Problem::None;
    std::string error;   // for the log: may name the disc's file and give the raw reason
    std::string shown;   // for the page: "File k of n ..." - never a file name
    uint64_t needBytes=0,freeBytes=0,leftoversRemoved=0;int filesToRead=0;uint64_t bytesToRead=0;
    std::vector<FileResult> files;std::vector<Pin> stillMissing;
    int kept() const {int n=0;for(const auto& f:files)n+=f.kept;return n;}
};
inline const char* SHOWN_DAMAGED=" is damaged in your disc image.";
inline const char* SHOWN_INCOMPLETE=" is incomplete in your disc image.";
inline const char* SHOWN_NOT_READ=" could not be read from your disc image.";
inline const char* SHOWN_NOT_WRITTEN=" could not be written to this device's storage.";
inline const char* SHOWN_NOT_KEPT=" was copied but could not be put in place on this device's storage.";
inline const char* SHOWN_NOT_PREPARED="The folder for the game's files could not be prepared on this device's storage.";
inline const char* SHOWN_UNEXPECTED="Copying stopped unexpectedly.";
inline const char* SHOWN_DAMAGED_MANY=" are damaged in your disc image.";
inline std::string shown(int index,int count,const char* what) {return "File "+std::to_string(index+1)+" of "+std::to_string(count)+what;}
inline std::vector<FileResult> failedCheck(const std::vector<FileResult>& files,const std::vector<Pin>& missingPins) {
    std::vector<uint64_t> sizes;for(const auto& p:missingPins)sizes.push_back(p.bytes);
    std::vector<FileResult> out;
    for(const auto& f:files)if(f.verdict==Verdict::NotPinned){auto it=std::find(sizes.begin(),sizes.end(),f.bytes);if(it!=sizes.end()){sizes.erase(it);out.push_back(f);}}
    return out;
}
inline std::string shownDamaged(const std::vector<FileResult>& damaged,int count) {
    if(damaged.size()==1)return shown(damaged[0].index,count,SHOWN_DAMAGED);
    std::string out="Files ";
    for(size_t i=0;i<damaged.size();++i){if(i>0)out+=i==damaged.size()-1?" and ":", ";out+=std::to_string(damaged[i].index+1);}
    return out+" of "+std::to_string(count)+SHOWN_DAMAGED_MANY;
}
inline uint64_t bytesOf(const std::vector<Pin>& pins) {uint64_t t=0;for(const auto& p:pins)t+=p.bytes;return t;}

/** Where the bytes go: the device (DiskStore) or nowhere (HashOnlyStore, the proof tool). Every decision is made by the same lines either way. */
struct Store {
    virtual ~Store()=default;
    virtual bool writes() const =0;
    virtual uint64_t prepare() =0;                          // folders in place, left-overs removed; the bytes removed
    virtual Sink open() =0;                                 // a new, empty part file (its write failures are WriteFailed)
    virtual void finish() =0;                               // on storage and closed
    virtual void keep(const std::string& sha) =0;           // the part file becomes inputs/<sha>.bin in one step
    virtual void discard() noexcept =0;                     // the part file removed
    virtual std::set<std::string> installedPins(const std::vector<Pin>& pins) =0;
};
/** Regular files *.part directly in disc-staging/ (links are not followed and not counted). */
inline std::vector<std::string> leftovers(const std::string& root) {
    std::vector<std::string> out;const std::string staging=root+"/"+STAGING;struct stat st{};
    if(::lstat(staging.c_str(),&st)!=0 || !S_ISDIR(st.st_mode))return out;
    if(DIR* d=::opendir(staging.c_str())) {
        while(dirent* e=::readdir(d)) {
            const std::string n=e->d_name;if(n.size()<5 || n.compare(n.size()-5,5,".part")!=0)continue;
            const std::string p=staging+"/"+n;struct stat fs{};if(::lstat(p.c_str(),&fs)==0 && S_ISREG(fs.st_mode))out.push_back(p);
        }
        ::closedir(d);
    }
    return out;
}
inline uint64_t leftoverBytes(const std::string& root) {uint64_t b=0;for(const auto& p:leftovers(root)){uint64_t n=0;if(regularFile(p,n))b+=n;}return b;}
inline uint64_t clearLeftovers(const std::string& root) {
    uint64_t removed=0;
    for(const auto& p:leftovers(root)){uint64_t n=0;regularFile(p,n);if(::unlink(p.c_str())!=0)throw IoError(errnoText("cannot remove a left-over part file",errno));removed+=n;}
    return removed;
}
/** root: <Library/r>/private-trace. */
struct DiskStore : Store {
    std::string root,inputs,staging,part;int fd=-1;
    explicit DiskStore(std::string r):root(std::move(r)),inputs(root+"/"+INPUTS),staging(root+"/"+STAGING),part(staging+"/"+PART){}
    ~DiskStore() override {discard();}
    bool writes() const override {return true;}
    uint64_t prepare() override {makeDirectory(root);makeDirectory(inputs);makeDirectory(staging);return clearLeftovers(root);}
    Sink open() override {
        ::unlink(part.c_str()); // (whatever stood there - a killed run's part, a link - is removed first; nothing is written through a link)
        fd=::open(part.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0)throw WriteFailed(errnoText("cannot create the part file",errno));
        const int f=fd;
        return [f](const uint8_t* d,size_t n){
            while(n){const ssize_t w=::write(f,d,n);if(w<0){if(errno==EINTR)continue;throw WriteFailed(errnoText("write failed",errno));}d+=w;n-=size_t(w);}
        };
    }
    void finish() override {
        const int f=fd;fd=-1;
        const int r=::fsync(f),err=errno; // (the bytes are on storage before the file gets its final name)
        ::close(f);
        if(r!=0)throw WriteFailed(errnoText("could not flush the copy to storage",err));
    }
    void keep(const std::string& sha) override {
        const std::string target=inputs+"/"+sha+".bin";struct stat st{};
        if(::lstat(target.c_str(),&st)==0 && S_ISDIR(st.st_mode))throw IoError("a folder stands under the name "+sha+".bin");
        ::chmod(part.c_str(),0400);
        if(::rename(part.c_str(),target.c_str())!=0)throw IoError(errnoText("could not rename the copy",errno)); // (one step; a file of another size that stood there is replaced)
        syncDirectory(inputs);
    }
    void discard() noexcept override {if(fd>=0){::close(fd);fd=-1;}::unlink(part.c_str());}
    std::set<std::string> installedPins(const std::vector<Pin>& pins) override {return installed(inputs,pins);}
};
/** The proof tool's --hash-only store: nothing written; inputsAlready (read only) counts as in place; kept files remembered. */
struct HashOnlyStore : Store {
    std::string inputsAlready;std::set<std::string> made;
    explicit HashOnlyStore(std::string dir):inputsAlready(std::move(dir)){}
    bool writes() const override {return false;}
    uint64_t prepare() override {return 0;}
    Sink open() override {return [](const uint8_t*,size_t){};}
    void finish() override {}
    void keep(const std::string& sha) override {made.insert(sha);}
    void discard() noexcept override {}
    std::set<std::string> installedPins(const std::vector<Pin>& pins) override {
        auto out=installed(inputsAlready,pins);for(const auto& p:pins)if(made.count(p.sha256))out.insert(p.sha256);return out;
    }
};

/** fileIndex: 0-based among the files to read. */
struct Progress {
    std::function<void(uint64_t done,uint64_t total,int fileIndex,int files,const std::string& name)> update=[](uint64_t,uint64_t,int,int,const std::string&){};
    std::function<bool()> cancelled=[]{return false;};
};

namespace detail {
inline Outcome outcome(bool complete,bool stopped,Problem problem,std::string error,std::string shown_,uint64_t need,uint64_t free_,uint64_t leftovers_,
                       const Plan* plan,const std::vector<FileResult>& files,std::vector<Pin> missingPins) {
    Outcome o;o.complete=complete;o.stopped=stopped;o.problem=problem;o.error=std::move(error);o.shown=std::move(shown_);o.needBytes=need;o.freeBytes=free_;
    o.leftoversRemoved=leftovers_;o.filesToRead=plan?int(plan->read.size()):0;o.bytesToRead=plan?plan->bytes:0;o.files=files;o.stillMissing=std::move(missingPins);return o;
}
inline Outcome noSpace(const std::string& what,uint64_t need,uint64_t free_,uint64_t left,const Plan& plan,const std::vector<FileResult>& files,std::vector<Pin> m) {
    const std::string amounts=": needs "+gigabytes(need,true)+", free "+gigabytes(free_,false)+".";
    return outcome(false,false,Problem::NoSpace,what+amounts,"Not enough space"+amounts,need,free_,left,&plan,files,std::move(m));
}
inline Outcome failed(Problem p,const std::string& error,const std::string& shown_,uint64_t left,const Plan* plan,const std::vector<FileResult>& files,std::vector<Pin> m) {
    return outcome(false,false,p,error,shown_,0,0,left,plan,files,std::move(m));
}
/** Reads one candidate into the store while hashing it; the part file is removed on any failure. */
inline std::string copyOne(const Candidate& c,Store& store,const Progress& progress,uint64_t doneBefore,uint64_t total,int index,int count) {
    bool finished=false;
    try {
        const Sink out=store.open();
        const Watch watch=[&](uint64_t done){progress.update(doneBefore+done,total,index,count,c.name);return !progress.cancelled();};
        const std::string sha=sha256Candidate(c,&out,&watch);
        store.finish();finished=true;return sha;
    } catch(...) {if(!finished)store.discard();throw;}
}
inline Outcome runLocked(const Disc& disc,const std::vector<Pin>& expected,const std::vector<Pin>& allPins,const std::set<std::string>& notTrusted,
                         const std::string& root,Store& store,const Progress& progress,const Space& space,const std::function<void(const FileResult&)>& report) {
    std::vector<FileResult> files;
    if(!disc.usable())return outcome(false,false,Problem::DiscCopyBad,disc.logged(),disc.message,0,0,0,nullptr,files,expected);
    std::set<std::string> inst=store.installedPins(allPins);for(const auto& d:notTrusted)inst.erase(d);
    // Asked before anything is written: does this disc hold a file of the right size for each of its entries?
    const Plan own=plan(disc.candidates,expected,inst);
    if(!own.shortSizes.empty()) {
        const std::string e="This disc image lacks "+std::to_string(own.shortSizes.size())+" of the "+std::to_string(expected.size())+" files this disc should hold.";
        return failed(Problem::DiscCopyBad,e,e,0,nullptr,files,notIn(expected,inst));
    }
    const Plan pl=plan(disc.candidates,allPins,inst);
    const int count=int(pl.read.size());uint64_t left=0;std::set<std::string> keptNow;
    try {
        if(count>0) {
            left=store.prepare(); // (a stopped or killed run's left-over is this copy's own: removed before the free space is asked for)
            if(store.writes()) {
                const uint64_t need=bytesOf(notIn(expected,inst))+RESERVE_BYTES,free_=space(existingAncestor(root));
                if(need>free_)return noSpace("Not enough space",need,free_,left,pl,files,notIn(expected,inst));
            }
        }
        uint64_t done=0;
        for(int i=0;i<count;++i) {
            const Candidate& c=*pl.read[size_t(i)];
            if(progress.cancelled())return outcome(false,true,Problem::None,"","",0,0,left,&pl,files,notIn(expected,inst));
            if(store.writes() && i>0) { // (other apps use the same storage while this runs: every file starts with the same check)
                const uint64_t need=std::max(bytesOf(notIn(expected,inst)),c.bytes)+RESERVE_BYTES,free_=space(existingAncestor(root));
                if(need>free_)return noSpace("Not enough space left for "+c.name,need,free_,left,pl,files,notIn(expected,inst));
            }
            progress.update(done,pl.bytes,i,count,c.name);
            std::string sha;
            try {sha=copyOne(c,store,progress,done,pl.bytes,i,count);}
            catch(const Stopped&){return outcome(false,true,Problem::None,"","",0,0,left,&pl,files,notIn(expected,inst));}
            catch(const WriteFailed& e) {
                const uint64_t free_=space(existingAncestor(root)),need=std::max(bytesOf(notIn(expected,inst)),c.bytes)+RESERVE_BYTES;
                if(need>free_)return noSpace("This device ran out of space while copying "+c.name,need,free_,left,pl,files,notIn(expected,inst));
                return failed(Problem::Storage,"Could not write the copy of "+c.name+": "+reason(e)+".",shown(i,count,SHOWN_NOT_WRITTEN),left,&pl,files,notIn(expected,inst));
            }
            catch(const StfsDamaged& e){return failed(Problem::DiscCopyBad,c.name+" is damaged in your disc image ("+reason(e)+").",shown(i,count,SHOWN_DAMAGED),left,&pl,files,notIn(expected,inst));}
            catch(const Eof& e){return failed(Problem::DiscCopyBad,c.name+" is incomplete in your disc image ("+reason(e)+").",shown(i,count,SHOWN_INCOMPLETE),left,&pl,files,notIn(expected,inst));}
            catch(const IoError& e){return failed(Problem::SourceUnreadable,c.name+" could not be read from your disc image ("+reason(e)+").",shown(i,count,SHOWN_NOT_READ),left,&pl,files,notIn(expected,inst));}
            Verdict verdict=pl.judge(c.bytes,sha);
            if(verdict==Verdict::Wanted && inst.count(sha))verdict=Verdict::AlreadyInstalled; // (the same content offered twice by one disc)
            bool kept=false;
            if(verdict==Verdict::Wanted) {
                try {store.keep(sha);kept=true;inst.insert(sha);keptNow.insert(sha);}
                catch(const std::exception& e) {
                    store.discard();
                    return failed(Problem::Storage,"Could not put "+c.name+" in place: "+reason(e)+".",shown(i,count,SHOWN_NOT_KEPT),left,&pl,files,notIn(expected,inst));
                }
            } else store.discard();
            FileResult r{i,count,c.name,c.bytes,sha,verdict,kept};files.push_back(r);if(report)report(r);
            done+=c.bytes;
        }
        if(count>0)progress.update(pl.bytes,pl.bytes,count,count,"");
        // Asked after the copy, of what is really in place.
        std::set<std::string> after=store.installedPins(allPins);
        for(const auto& d:notTrusted)if(!keptNow.count(d))after.erase(d);
        const auto miss=notIn(expected,after);
        if(!miss.empty()) {
            const std::string error="This disc image did not provide "+std::to_string(miss.size())+" of the "+std::to_string(expected.size())+" files this disc should hold.";
            const auto damaged=failedCheck(files,miss);
            if(damaged.empty())return failed(Problem::DiscCopyBad,error,error,left,&pl,files,miss);
            std::string names;for(const auto& f:damaged)names+=(names.empty()?"":", ")+f.name;
            std::string sh=shownDamaged(damaged,count);
            if(damaged.size()<miss.size()){const size_t other=miss.size()-damaged.size();sh+=" It did not provide "+(other==1?std::string("1 other file"):std::to_string(other)+" other files")+" this disc should hold.";}
            return failed(Problem::DiscCopyBad,error+" Failed its check: "+names+".",sh,left,&pl,files,miss);
        }
        return outcome(true,false,Problem::None,"","",0,0,left,&pl,files,miss);
    } catch(const IoError& e) {
        store.discard();
        return failed(Problem::Storage,"Could not prepare the game files' folder: "+reason(e)+".",SHOWN_NOT_PREPARED,left,&pl,files,notIn(expected,inst));
    } catch(const std::exception& e) {
        store.discard();
        return failed(Problem::Other,"Copying stopped unexpectedly: "+reason(e)+".",SHOWN_UNEXPECTED,left,&pl,files,notIn(expected,inst));
    }
}
} // namespace detail

/**
 * Copies what disc offers into root (<Library/r>/private-trace): expected = the pins this disc is to provide; allPins = every pin from
 * a disc (a file of that size and SHA-256 is kept); notTrusted = pins in place by name and size but known not to be the pinned file.
 * Never throws. Resumes by construction: pins already in place are not read again; a killed run's part file is removed first.
 */
inline Outcome run(const Disc& disc,const std::vector<Pin>& expected,const std::vector<Pin>& allPins,const std::set<std::string>& notTrusted,
                   const std::string& root,Store& store,const Progress& progress,const Space& space=diskSpace,
                   const std::function<void(const FileResult&)>& report={}) {
    if(!begin())return detail::failed(Problem::Busy,BUSY_MESSAGE,BUSY_MESSAGE,0,nullptr,{},expected);
    struct Unlock {~Unlock(){end();}} unlock;
    return detail::runLocked(disc,expected,allPins,notTrusted,root,store,progress,space,report);
}
/** For a first screen: removes what a killed copy left in disc-staging/, unless a copy runs now. Never throws. */
inline uint64_t removeLeftOvers(const std::string& root) {
    if(!begin())return 0;
    struct Unlock {~Unlock(){end();}} unlock;
    try {return clearLeftovers(root);} catch(...) {return 0;}
}

} // namespace recomp::apple::discimport
