# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import generate_bridge


@unittest.skipUnless(shutil.which('clang++'),'requires a host C++ compiler')
class GeneratedBridge(unittest.TestCase):
    def test_generated_adapters_forward_arguments_and_preserve_void_returns(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            generate_bridge.generate('PPC_EXTERN_FUNC(__imp__Value);\nPPC_EXTERN_FUNC(__imp__Void);\nPPC_EXTERN_FUNC(__imp__Absent);',
                                     {'__imp__Value':True,'__imp__Void':False},root)
            (root/'ppc_config.h').write_text('#pragma once\n')
            (root/'ppc_context.h').write_text('''#pragma once
#include <cstdint>
struct Register { std::uint64_t u64=0; };
struct PPCContext { Register r1,r3,r4,r5,r6,r7,r8,r9,r10; };
''')
            (root/'guest_import_registry.h').write_text('''#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace recomp::bootstrap {
enum class GuestImportDispatchState { Handled, NotRegistered };
struct GuestImportCall { std::array<std::uint64_t,8> arguments{}; std::uint64_t returnValue=0; std::uint32_t stackPointer=0; };
struct Result { GuestImportDispatchState state=GuestImportDispatchState::Handled; bool handled() const { return true; } };
struct GuestImportRegistry {
  std::uint32_t stack=0; std::uint64_t argument=0; std::string_view name;
  Result dispatch(std::string_view n,GuestImportCall& call) { name=n;stack=call.stackPointer;argument=call.arguments[0];call.returnValue=42;return {}; }
};
void setGuestImportEntryHook(void (*)()) noexcept;
}
''')
            (root/'guest_execution_guard.h').write_text('''#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>
namespace recomp::bootstrap {
struct GuestImportArguments { std::uint64_t stack;std::array<std::uint64_t,8> arguments; };
struct GuestExecutionGuard {
 static void captureImportArguments(GuestImportArguments) {}
 [[noreturn]] static void stopForUnimplementedImport(std::string_view) { std::abort(); }
 [[noreturn]] static void stopForRejectedImport(std::string_view) { std::abort(); }
}; }
''')
            (root/'generated_guest_import_bridge.h').write_text('''#pragma once
#include "guest_import_registry.h"
extern "C" void recomp_bind_generated_guest_import_registry(recomp::bootstrap::GuestImportRegistry*,std::uint8_t*) noexcept;
extern "C" void recomp_unbind_generated_guest_import_registry() noexcept;
''')
            (root/'main.cpp').write_text('''#include "ppc_context.h"
#include "generated_guest_import_bridge.h"
#include <atomic>
#include <cassert>
extern "C" { std::atomic<bool> recomp_imports_fast{true},recomp_v312_release{true}; }
namespace recomp::bootstrap { const char* exchangeGuestHostActivity(const char*) noexcept { return nullptr; } }
void __imp__Value(PPCContext&,std::uint8_t*);
void __imp__Void(PPCContext&,std::uint8_t*);
int main() {
 recomp::bootstrap::GuestImportRegistry registry;std::uint8_t base{};PPCContext ctx;
 recomp_bind_generated_guest_import_registry(&registry,&base);
 ctx.r1.u64=128;ctx.r3.u64=7;__imp__Value(ctx,&base);
 assert(ctx.r3.u64==42 && registry.stack==128 && registry.argument==7);
 ctx.r3.u64=9;__imp__Void(ctx,&base);assert(ctx.r3.u64==9 && registry.name=="__imp__Void");
 recomp_unbind_generated_guest_import_registry();
}
''')
            executable=root/'test'
            subprocess.run(['clang++','-std=c++20','-I'+str(root),str(root/'generated_guest_import_bridge.cpp'),
                            str(root/'remaining_xbox_import_guarded_stops.cpp'),str(root/'main.cpp'),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)


if __name__=='__main__':
    unittest.main()
