#pragma once
namespace recomp::gpu {
struct NativePrimaryClearResult {bool completed=false,presentationValid=false;};
// Preserving a different surface is not executing the requested clear. This
// separation keeps an uncaptured offscreen clear eligible for its real fallback
// executor (or refusal), regardless of the display image's validity.
template<class Submit>
NativePrimaryClearResult executeNativePrimaryClear(bool captured,bool preserveOtherPresentation,Submit&& submit) {
    const bool completed=captured && submit();
    return {completed,completed || (!captured && preserveOtherPresentation)};
}
}
