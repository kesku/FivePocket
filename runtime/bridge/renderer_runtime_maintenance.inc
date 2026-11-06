// Exact live recording -> retained target -> later runtime dispatch connection.
// Included with production owners, and by the lifecycle contract test.
bool handoffRendererCompletedSceneTarget() {
    if(activeEndQueue || activeSceneQueue || !liveNativeScene ||
       liveNativeScene->phase!=LiveNativeScene::Phase::FinalRetired)return false;
    if(!completedNativeSceneTarget.retain(liveNativeScene->device,inlineGraphics,pendingNativeSceneTarget))return false;
    liveNativeScene.reset();pendingNativeSceneTarget.reset();return true;
}
bool submitRendererRuntimeMaintenance(std::uint32_t writeWord) {
    // Successful EndTiling has released liveNativeScene and all CPU captures.
    // An active/new recording may never reuse the previous generation's target.
    if(activeEndQueue || activeSceneQueue || liveNativeScene || pendingNativeSceneTarget ||
       tileRecordingDevice.load() || !inlineRegistry || !inlineGraphics || !originalRecordingGate) {
        rendererLog("RENDER_RUNTIME_MAINTENANCE admission=recording-or-unbound write=%u gameplay_verified=0\n",writeWord);
        return false;
    }
    const auto* target=completedNativeSceneTarget.target(inlineGraphics);
    if(!target) {
        rendererLog("RENDER_RUNTIME_MAINTENANCE admission=no-completed-target write=%u gameplay_verified=0\n",writeWord);
        return false;
    }
    const auto device=completedNativeSceneTarget.device();
    const bool completed=inlineRegistry->submitNativeRuntimeMaintenancePoints(device,writeWord,originalRecordingGate,
        [&](const auto& root,const auto& graph,const auto& publish) {
            // Registry holds the current allocation lock; the exclusive guest
            // gate prevents new recording, owner changes and generated stores.
            return inlineGraphics->executeRuntimeMaintenancePoints(root,graph,*target,publish);
        });
    rendererLog("RENDER_RUNTIME_MAINTENANCE completed=%u write=%u device=%08x gameplay_verified=0\n",
        unsigned(completed),writeWord,device);
    return completed;
}
