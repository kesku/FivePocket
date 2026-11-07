// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios_bundle_seed.h (Claude 2026-11-05; the builder's iOS brief, option (b) - owner: "the ipa doesn't need to be small"): the builder puts
// its own parts inside the .app - the template run (run-<id>/decoded.image.bin), modules/boot, the six file catalogs (inputs/<sha>.bin);
// never a disc file (owner: "the data needs to be imported from the disc no matter what": the 18 come only through the on-device disc
// import, ios_disc_import_view.h). On launch the shell seeds them into <home>/Library/r, where the runtime reads
// them: the runtime opens its files through an O_NOFOLLOW directory chain and checks them as resident files (ios_run_files.cpp), so a
// link into the bundle would be refused; the seed places real files.
//
// The manifest is pinned: its SHA-256 is compiled into the shell (claude_build.py: RECOMP_PAYLOAD_MANIFEST_SHA256, unit 18), and the seed
// takes only a manifest with exactly that hash - a payload of another edition or another build is refused; so every file's size and
// SHA-256 come from the compiled pin. A manifest that lists a disc pin is refused.
// payload/manifest.txt in the .app, one line per file: "<sha256> <bytes> <path under Library/r>" (64 lower-case hex digits, decimal
// size, a relative path of [A-Za-z0-9._-] parts joined by '/', no '..', no leading '/'); the file itself at payload/r/<path>.
// Per file, in manifest order:
//  - already in place (a regular file of that size, not a link): kept as it is - the rule of Disc2Discs.installed: a file gets its name
//    only after it was verified (here: cloned from the sealed bundle, or copied with its SHA-256 checked);
//  - else cloned (APFS clonefile: copy on write, no space taken, immediate) to <parent>/.seed.part, made read-only, renamed into place;
//    where cloning is not possible (another volume, a file system without clones) copied the same way while its SHA-256 is checked;
//  - a path that crosses a link, a folder standing under a file's name, a size or SHA-256 that differs: refused, nothing renamed.
// Never removes or changes a file it did not create; never follows a link. Pure C++ (POSIX + CommonCrypto); host tests in
// tests_bundle_seed.cpp.
#include "ios_disc_import.h"
#include <sys/clonefile.h>
#include <fstream>
#include <sstream>

namespace recomp::apple::bundleseed {
namespace di=recomp::apple::discimport;

struct Entry {std::string sha256;uint64_t bytes=0;std::string path;};
struct Result {
    bool ok=false;int seeded=0,kept=0,cloned=0,copied=0;uint64_t bytes=0;std::string error;  // error: for the log (it may name a path)
};

inline bool safePath(const std::string& p) {
    if(p.empty() || p.size()>1024 || p.front()=='/' || p.back()=='/')return false;
    size_t s=0;
    for(;;) {
        const size_t e=p.find('/',s);const std::string part=p.substr(s,e==std::string::npos?std::string::npos:e-s);
        if(part.empty() || part=="." || part==".." || part[0]=='.')return false;
        for(char c:part)if(!(std::isalnum((unsigned char)c) || c=='.' || c=='_' || c=='-'))return false;
        if(e==std::string::npos)return true;
        s=e+1;
    }
}
/** The manifest's lines; false (with why) on any malformed line, a repeated path or no line at all. */
inline bool parse(const std::string& text,std::vector<Entry>& out,std::string& why) {
    std::istringstream in(text);std::string line;std::set<std::string> seen;int n=0;
    while(std::getline(in,line)) {
        ++n;if(!line.empty() && line.back()=='\r')line.pop_back();
        if(line.empty())continue;
        std::istringstream l(line);Entry e;std::string bytes,extra;
        if(!(l>>e.sha256>>bytes>>e.path) || (l>>extra) || !di::isSha256(e.sha256) || bytes.empty() || bytes.size()>15 ||
           bytes.find_first_not_of("0123456789")!=std::string::npos || !safePath(e.path)){why="manifest line "+std::to_string(n)+" is malformed";return false;}
        e.bytes=std::stoull(bytes);
        if(!seen.insert(e.path).second){why="manifest line "+std::to_string(n)+" repeats a path";return false;}
        out.push_back(e);
    }
    if(out.empty()){why="the manifest lists no file";return false;}
    return true;
}
/** Makes root/<dir parts of path> as real directories, never through a link (makeDirectory refuses a link or file in the way). */
inline void makeParents(const std::string& root,const std::string& path) {
    const size_t s=path.rfind('/');if(s==std::string::npos){di::makeDirectory(root);return;}
    di::makeDirectory(root+"/"+path.substr(0,s));
}
/** SHA-256 of a regular file (not through a link), its size into bytes; throws on a read failure. */
inline std::string fileSha256(const std::string& p,uint64_t& bytes) {
    const int fd=::open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)throw di::IoError(di::errnoText("cannot open "+p,errno));
    di::Sha256 sha;std::vector<uint8_t> buf(1u<<20);bytes=0;
    for(;;){const ssize_t r=::read(fd,buf.data(),buf.size());if(r<0){if(errno==EINTR)continue;const int e=errno;::close(fd);throw di::IoError(di::errnoText("read failed",e));}
        if(r==0)break;sha.update(buf.data(),size_t(r));bytes+=uint64_t(r);}
    ::close(fd);return sha.hexDigest();
}
/** Copies src to dst (O_EXCL|O_NOFOLLOW) while hashing; returns the SHA-256; fsync before close. */
inline std::string copyFile(const std::string& src,const std::string& dst,uint64_t& bytes) {
    const int in=::open(src.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(in<0)throw di::IoError(di::errnoText("cannot open "+src,errno));
    const int out=::open(dst.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(out<0){const int e=errno;::close(in);throw di::IoError(di::errnoText("cannot create "+dst,e));}
    di::Sha256 sha;std::vector<uint8_t> buf(1u<<20);bytes=0;
    auto fail=[&](const std::string& what){const int e=errno;::close(in);::close(out);::unlink(dst.c_str());throw di::IoError(di::errnoText(what,e));};
    for(;;) {
        const ssize_t r=::read(in,buf.data(),buf.size());if(r<0){if(errno==EINTR)continue;fail("read failed");}
        if(r==0)break;
        sha.update(buf.data(),size_t(r));bytes+=uint64_t(r);
        for(ssize_t w=0;w<r;){const ssize_t k=::write(out,buf.data()+w,size_t(r-w));if(k<0){if(errno==EINTR)continue;fail("write failed");}w+=k;}
    }
    if(::fsync(out)!=0)fail("could not flush the copy");
    ::close(in);::close(out);return sha.hexDigest();
}
/** Clone hook (tests force the copy path): clonefile(src, dst, CLONE_NOFOLLOW) == 0. */
using Clone=std::function<bool(const std::string&,const std::string&)>;
inline bool apfsClone(const std::string& src,const std::string& dst) {return ::clonefile(src.c_str(),dst.c_str(),CLONE_NOFOLLOW|CLONE_NOOWNERCOPY)==0;}

/**
 * Seeds every manifest entry from payloadDir (the .app's payload/) into root (<home>/Library/r). verifyClones: also hash a cloned
 * file (it reads the bytes; the bundle is sealed by its code signature, so the default is size only for clones). Never throws.
 */
inline Result seed(const std::string& payloadDir,const std::string& root,const std::string& pinnedManifestSha256,const Clone& clone=apfsClone,
                   bool verifyClones=false) {
    Result r;std::vector<Entry> entries;
    try {
        if(!di::isSha256(pinnedManifestSha256)){r.error="this app carries no payload pin";return r;}
        std::ifstream f(payloadDir+"/manifest.txt",std::ios::binary);
        if(!f){r.error="no payload manifest";return r;}
        std::stringstream ss;ss<<f.rdbuf();const std::string text=ss.str();
        {di::Sha256 h;h.update(reinterpret_cast<const uint8_t*>(text.data()),text.size());
         if(h.hexDigest()!=pinnedManifestSha256){r.error="the payload's manifest is not the one this app was built with";return r;}}
        if(!parse(text,entries,r.error))return r;
        for(const auto& e:entries)for(const auto& p:di::catalog::pins())
            if(e.sha256==p.sha256){r.error="the payload carries a disc file ("+e.path+"): disc files come only through the disc import";return r;}
        for(const auto& e:entries) {
            const std::string target=root+"/"+e.path,source=payloadDir+"/r/"+e.path;
            uint64_t have=0;struct stat st{};
            if(::lstat(target.c_str(),&st)==0) {
                if(S_ISDIR(st.st_mode)){r.error="a folder stands under the name "+e.path;return r;}
                if(di::regularFile(target,have) && have==e.bytes){++r.kept;continue;}
            }
            uint64_t srcBytes=0;
            if(!di::regularFile(source,srcBytes) || srcBytes!=e.bytes){r.error="the bundle's "+e.path+" is missing or not of its listed size";return r;}
            makeParents(root,e.path);
            const size_t s=e.path.rfind('/');
            const std::string part=root+"/"+(s==std::string::npos?std::string():e.path.substr(0,s)+"/")+".seed.part";
            ::unlink(part.c_str()); // (a killed run's part; never a link followed: unlink removes the name only)
            bool cloned=false;std::string sha;
            if(clone(source,part)) {
                cloned=true;uint64_t b=0;
                if(!di::regularFile(part,b) || b!=e.bytes){::unlink(part.c_str());r.error="the clone of "+e.path+" is not of its listed size";return r;}
                if(verifyClones){sha=fileSha256(part,b);if(sha!=e.sha256){::unlink(part.c_str());r.error=e.path+" in the bundle is not the listed file";return r;}}
            } else {
                uint64_t b=0;sha=copyFile(source,part,b);
                if(b!=e.bytes || sha!=e.sha256){::unlink(part.c_str());r.error=e.path+" in the bundle is not the listed file";return r;}
            }
            ::chmod(part.c_str(),0400);
            if(::rename(part.c_str(),target.c_str())!=0){const int er=errno;::unlink(part.c_str());r.error=di::errnoText("could not put "+e.path+" in place",er);return r;}
            di::syncDirectory(target.substr(0,target.rfind('/')));
            ++r.seeded;r.bytes+=e.bytes;(cloned?r.cloned:r.copied)++;
        }
        r.ok=true;return r;
    } catch(const std::exception& e) {r.error=di::reason(e);return r;}
}
} // namespace recomp::apple::bundleseed
