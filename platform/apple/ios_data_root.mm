// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/data_root.h"
#import <Foundation/Foundation.h>
#include <limits.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace recomp::apple {
bool prepareIosDataRoot(std::string& result,std::string& error) {
    result.clear();error.clear();
    if (![NSThread isMainThread]) {error="data root setup requires main thread";return false;}
    NSURL* library=[NSFileManager.defaultManager URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask].firstObject;
    char actual[PATH_MAX];
    if (!library || !realpath(library.fileSystemRepresentation,actual)) {error="cannot resolve app Library";return false;}
    std::string candidate;
    if (!dataRootPath(actual,candidate)) {error="canonical data root exceeds runtime path contract";return false;}
    struct stat metadata{};
    if (lstat(candidate.c_str(),&metadata)) {
        if (errno!=ENOENT || mkdir(candidate.c_str(),0700) || lstat(candidate.c_str(),&metadata)) {
            error="cannot create private data directory";return false;
        }
    }
    if (!S_ISDIR(metadata.st_mode) || S_ISLNK(metadata.st_mode) ||
        !realpath(candidate.c_str(),actual) || candidate!=actual) {
        error="data directory is not an exact canonical directory";return false;
    }
    NSURL* url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:candidate.c_str()] isDirectory:YES];
    NSError* failure=nil;
    if (![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&failure]) {
        error="cannot exclude private game data from backup";return false;
    }
    NSNumber* excluded=nil;
    if (![url getResourceValue:&excluded forKey:NSURLIsExcludedFromBackupKey error:&failure] || !excluded.boolValue) {
        error="backup exclusion did not verify";return false;
    }
    result=std::move(candidate);return true;
}
}
