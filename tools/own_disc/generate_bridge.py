#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate PPC import adapters from the owned executable's import declarations."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
PROLOGUE = r'''// Generated from this build's import declarations; do not distribute.
#include "ppc_config.h"
#include "ppc_context.h"
#include "guest_import_registry.h"
#include "guest_execution_guard.h"
#include "generated_guest_import_bridge.h"
#include <array>
#include <atomic>
#include <optional>
#include <string_view>

namespace recomp::bootstrap {
const char* exchangeGuestHostActivity(const char*) noexcept;
std::atomic<void (*)()> guestImportEntryHook{nullptr};
void setGuestImportEntryHook(void (*hook)()) noexcept {
    guestImportEntryHook.store(hook,std::memory_order_release);
}
}
extern "C" std::atomic<bool> recomp_imports_fast;
extern "C" std::atomic<bool> recomp_v312_release;
namespace {
using recomp::bootstrap::GuestImportRegistry;
using recomp::bootstrap::GuestImportCall;
std::atomic<GuestImportRegistry*> boundRegistry{nullptr};
std::atomic<std::uint8_t*> boundGuestBase{nullptr};
[[noreturn]] void bridgeTrap() noexcept { __builtin_trap(); }
struct HostActivityScope {
    const char* previous;
    explicit HostActivityScope(const char* label) noexcept
        : previous(recomp::bootstrap::exchangeGuestHostActivity(label)) {}
    ~HostActivityScope() { recomp::bootstrap::exchangeGuestHostActivity(previous); }
};
void dispatchBoundedImport(std::string_view name,bool writesReturnValue,PPCContext& ctx,std::uint8_t* base) {
    auto* registry=boundRegistry.load(std::memory_order_acquire);
    if (!registry || !base || base!=boundGuestBase.load(std::memory_order_acquire)) bridgeTrap();
    GuestImportCall call{};
    call.stackPointer=static_cast<std::uint32_t>(ctx.r1.u64);
    call.arguments={{ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,
                     ctx.r7.u64,ctx.r8.u64,ctx.r9.u64,ctx.r10.u64}};
    std::optional<HostActivityScope> activity;
    if (!(recomp_imports_fast.load(std::memory_order_relaxed) &&
          recomp_v312_release.load(std::memory_order_relaxed))) activity.emplace("import");
    if (auto hook=recomp::bootstrap::guestImportEntryHook.load(std::memory_order_acquire)) hook();
    const auto result=registry->dispatch(name,call);
    activity.reset();
    if (!result.handled()) {
        recomp::bootstrap::GuestExecutionGuard::captureImportArguments({ctx.r1.u64,call.arguments});
        if (result.state==recomp::bootstrap::GuestImportDispatchState::NotRegistered)
            recomp::bootstrap::GuestExecutionGuard::stopForUnimplementedImport(name);
        recomp::bootstrap::GuestExecutionGuard::stopForRejectedImport(name);
    }
    if (writesReturnValue) ctx.r3.u64=call.returnValue;
}
}
extern "C" void recomp_bind_generated_guest_import_registry(
        recomp::bootstrap::GuestImportRegistry* registry,std::uint8_t* expectedGuestBase) noexcept {
    if (!registry || !expectedGuestBase) {
        boundRegistry.store(nullptr,std::memory_order_release);
        boundGuestBase.store(nullptr,std::memory_order_release);
        return;
    }
    boundGuestBase.store(expectedGuestBase,std::memory_order_release);
    boundRegistry.store(registry,std::memory_order_release);
}
extern "C" void recomp_unbind_generated_guest_import_registry() noexcept {
    boundRegistry.store(nullptr,std::memory_order_release);
    boundGuestBase.store(nullptr,std::memory_order_release);
}
'''
STOPS = r'''// Generated from this build's unresolved imports; do not distribute.
#include "ppc_config.h"
#include "ppc_context.h"
#include "guest_execution_guard.h"
'''


def generate(declarations: str, abi: dict, destination: Path) -> dict:
    names = sorted(set(re.findall(r'PPC_EXTERN_FUNC\((__imp__\w+)\)', declarations)))
    if not names:
        raise ValueError('the generated header contains no import declarations')
    bound, stops = [PROLOGUE], [STOPS]
    handled = 0
    for name in names:
        if name == '__imp__XamShowSigninUI':
            continue
        if name in abi:
            if type(abi[name]) is not bool:
                raise ValueError('import ABI return rule must be boolean: ' + name)
            bound.append(f'void {name}(PPCContext& ctx,std::uint8_t* base) {{\n'
                         f'    dispatchBoundedImport("{name}",{str(abi[name]).lower()},ctx,base);\n}}\n')
            handled += 1
        else:
            stops.append(f'void {name}(PPCContext& ctx,std::uint8_t* base) {{\n'
                         '    (void)base;\n'
                         '    recomp::bootstrap::GuestExecutionGuard::captureImportArguments({ctx.r1.u64,{{\n'
                         '        ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64,ctx.r8.u64,ctx.r9.u64,ctx.r10.u64}}});\n'
                         f'    recomp::bootstrap::GuestExecutionGuard::stopForUnimplementedImport("{name}");\n}}\n')
    if '__imp__XamShowSigninUI' in names:
        bound.append('#include "signin_ui_bridge.inc"\n')
    destination.mkdir(parents=True, exist_ok=True)
    for name,parts in [('generated_guest_import_bridge.cpp',bound),('remaining_xbox_import_guarded_stops.cpp',stops)]:
        path=destination/name
        text='\n'.join(parts)
        if not path.is_file() or path.read_text(encoding='utf-8')!=text:
            path.write_text(text,encoding='utf-8',newline='\n')
    return {'imports':len(names),'dispatched':handled,'guarded':len(names)-handled-int('__imp__XamShowSigninUI' in names)}


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--declarations',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args(argv)
    abi=json.loads((HERE/'config/import-abi.json').read_text(encoding='utf-8'))['writes_return']
    print(json.dumps(generate(args.declarations.read_text(encoding='utf-8'),abi,args.out)))


if __name__ == '__main__':
    main()
