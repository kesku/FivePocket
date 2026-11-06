#pragma once
#include <cstdint>

namespace recomp::gpu {
// Xenia registers.h RB_COLORCONTROL and spirv_shader_translator_rb.cc:
// compare output alpha TO RB_ALPHA_REF (not the other way round). NOT_EQUAL
// accepts NaN, ALWAYS accepts NaN, ordered comparisons do not. An alpha-test
// failure kills the fragment before it can modify stencil. Colour masking
// does not disable the alpha test. This helper grants no draw admission.
inline bool xenosAlphaPass(std::uint32_t colorControl,float alpha,float reference) {
    if(!(colorControl&8u))return true;
    switch(colorControl&7u) {
    case 0:return false;
    case 1:return alpha<reference;
    case 2:return alpha==reference;
    case 3:return alpha<=reference;
    case 4:return alpha>reference;
    case 5:return alpha!=reference;
    case 6:return alpha>=reference;
    case 7:return true;
    }
    return false;
}
}
