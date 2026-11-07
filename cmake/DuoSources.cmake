# SPDX-License-Identifier: GPL-3.0-or-later
set(FIVEPOCKET_RUNTIME_SOURCES
    runtime/device.cpp
    runtime/generic_renderer.cpp
    runtime/guest_address_space.cpp
    runtime/guest_cooperative_executor.cpp
    runtime/guest_execution_guard.cpp
    runtime/guest_import_registry.cpp
    runtime/guest_native_initialization.cpp
    runtime/guest_timebase.cpp
    runtime/indirect_call_resolver.cpp
    runtime/native_api_draw_target.cpp
    runtime/native_texture_image.cpp
    runtime/renderer_boundary_trace.cpp
    runtime/trace_runner.cpp
    runtime/vulkan_engine.cpp
    runtime/xenos_command_stream.cpp
    runtime/native_xma_context_pool.cpp
    runtime/host/guest_image_loader.cpp
    runtime/host/guest_thread_environment.cpp
    runtime/host/guest_thread_storage_pool.cpp
    runtime/host/guest_title_environment.cpp
    runtime/host/guest_xex_header.cpp
    runtime/host/xenos_texture_2d.cpp
    runtime/host/native_xma_frame_decoder.cpp
    runtime/bridge/guest_reservation_helpers.cpp)

set(FIVEPOCKET_APPLE_SOURCES
    app/duo/Main.mm
    app/duo/LauncherController.mm
    app/duo/LauncherCoordinator.mm
    app/duo/LauncherInputBridge.mm
    app/duo/AgentDiscardStream.cpp
    platform/apple/ios_frame_presenter.mm
    platform/apple/gamepad_input.mm
    platform/apple/ios_data_root.mm
    platform/apple/rgba_transfer.cpp
    platform/apple/asset_verifier.mm
    platform/apple/ios_request_plan.cpp
    platform/apple/ios_run_files.cpp
    platform/apple/ios_stage_preparation.cpp
    platform/apple/measurement_session.mm
    platform/apple/measurement_clock.mm
    platform/apple/callback_measurement.cpp
    platform/apple/counter_recording.cpp
    platform/apple/performance_sampler.mm
    platform/apple/frame_recording.cpp
    platform/apple/display_observation.mm
    platform/apple/display_recording_export.cpp
    platform/apple/launcher_settings_store.cpp
    platform/apple/launcher_display_options.cpp
    platform/apple/log.cpp
    platform/apple/audio_output.mm
    platform/apple/metal_surface.mm
    platform/apple/timeline.cpp
    platform/apple/ios_gpu_lifecycle.cpp
    platform/apple/native_gamepad_delivery.cpp
    platform/apple/private_gamepad_observer_v1.cpp
    platform/apple/private_gamepad_reader_selector.cpp)

function(fivepocket_source_definitions)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEFINITIONS" ${ARGN})
    set_property(SOURCE ${ARG_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS ${ARG_DEFINITIONS})
endfunction()

function(fivepocket_runtime_definitions)
    fivepocket_source_definitions(SOURCES
        runtime/device.cpp runtime/generic_renderer.cpp runtime/guest_address_space.cpp
        runtime/guest_cooperative_executor.cpp runtime/guest_execution_guard.cpp runtime/guest_import_registry.cpp
        runtime/guest_native_initialization.cpp runtime/guest_timebase.cpp runtime/indirect_call_resolver.cpp
        runtime/native_api_draw_target.cpp runtime/native_texture_image.cpp runtime/renderer_boundary_trace.cpp
        runtime/trace_runner.cpp runtime/vulkan_engine.cpp runtime/xenos_command_stream.cpp
        runtime/native_xma_context_pool.cpp runtime/bridge/guest_reservation_helpers.cpp
        DEFINITIONS RECOMP_GUEST_MEMORY_ATOMIC=1)
    fivepocket_source_definitions(SOURCES
        runtime/guest_cooperative_executor.cpp runtime/guest_execution_guard.cpp
        runtime/guest_import_registry.cpp runtime/guest_native_initialization.cpp
        runtime/trace_runner.cpp runtime/native_xma_context_pool.cpp
        DEFINITIONS RECOMP_GUEST_THREADS=2)
    fivepocket_source_definitions(SOURCES
        runtime/guest_cooperative_executor.cpp runtime/guest_import_registry.cpp
        runtime/renderer_boundary_trace.cpp runtime/trace_runner.cpp runtime/native_xma_context_pool.cpp
        DEFINITIONS RECOMP_DIAGNOSTIC_SKIP_BUILD=1)
    fivepocket_source_definitions(SOURCES
        runtime/device.cpp runtime/generic_renderer.cpp runtime/guest_cooperative_executor.cpp
        runtime/guest_import_registry.cpp runtime/indirect_call_resolver.cpp
        runtime/renderer_boundary_trace.cpp runtime/trace_runner.cpp
        DEFINITIONS RECOMP_THOR_EDITION=1 RECOMP_DUO_EDITION=1)
    fivepocket_source_definitions(SOURCES
        runtime/trace_runner.cpp runtime/native_xma_context_pool.cpp runtime/host/native_xma_frame_decoder.cpp
        DEFINITIONS RECOMP_NATIVE_XMA=1)
    fivepocket_source_definitions(SOURCES runtime/generic_renderer.cpp DEFINITIONS RECOMP_VKW_FLAT_SETS=1)
    fivepocket_source_definitions(SOURCES runtime/guest_import_registry.cpp
        DEFINITIONS RECOMP_IMPORT_TEXT_ON_FAILURE_ONLY=1 RECOMP_SAVE_REPLACE_DEFAULT=1)
    fivepocket_source_definitions(SOURCES runtime/native_api_draw_target.cpp DEFINITIONS RECOMP_DIAGNOSTIC_SHOW_FRAMES=1)
    fivepocket_source_definitions(SOURCES runtime/renderer_boundary_trace.cpp
        DEFINITIONS RECOMP_ENUMERATE_UNSUPPORTED_INLINE=1 RECOMP_DIAGNOSTIC_FAST_FRAMES=1
        RECOMP_DIAGNOSTIC_SHOW_FRAMES=1 RECOMP_DIAGNOSTIC_GENERIC_RENDER=1 RECOMP_CAPTURE_TILED_RECORDING=1)
    fivepocket_source_definitions(SOURCES runtime/trace_runner.cpp DEFINITIONS RECOMP_RENDERER_BOUNDARY_TRACE=1)
endfunction()

function(fivepocket_apple_definitions)
    fivepocket_source_definitions(SOURCES
        app/duo/Main.mm app/duo/LauncherController.mm app/duo/LauncherCoordinator.mm
        app/duo/LauncherInputBridge.mm platform/apple/ios_frame_presenter.mm
        platform/apple/asset_verifier.mm platform/apple/launcher_settings_store.cpp
        platform/apple/launcher_display_options.cpp platform/apple/audio_output.mm
        DEFINITIONS RECOMP_THOR_EDITION=1 RECOMP_DUO_EDITION=1)
    fivepocket_source_definitions(SOURCES app/duo/LauncherCoordinator.mm
        DEFINITIONS "RECOMP_PAYLOAD_MANIFEST_SHA256=\"${FIVEPOCKET_PAYLOAD_MANIFEST_SHA256}\"")
    fivepocket_source_definitions(SOURCES platform/apple/timeline.cpp
        DEFINITIONS "RECOMP_APPLE_TIMELINE_IMPLEMENTATION=\"diag_timeline.inc\"")
    fivepocket_source_definitions(SOURCES
        platform/apple/native_gamepad_delivery.cpp platform/apple/private_gamepad_observer_v1.cpp
        platform/apple/private_gamepad_reader_selector.cpp
        DEFINITIONS RECOMP_NATIVE_XMA=1 RECOMP_DIAGNOSTIC_SKIP_BUILD=1 RECOMP_GUEST_THREADS=2
        RECOMP_GUEST_MEMORY_ATOMIC=1 RECOMP_PRIVATE_TRACE_LIBRARY=1 SIMDE_ENABLE_NATIVE_ALIASES
        host_guest_trace_EXPORTS RECOMP_RENDERER_BOUNDARY_TRACE=1 _FORTIFY_SOURCE=2)
    set_property(SOURCE ${FIVEPOCKET_APPLE_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS OBJC_OLD_DISPATCH_PROTOTYPES=1)
endfunction()
