export const sourceLog = `[   0.001001] Frontend <Info> qt_common/qt_common.cpp:245:LogSystemInfo: Lichii Version: development-test-build
[   0.002002] Frontend <Info> qt_common/qt_common.cpp:261:LogSystemInfo: Host CPU: Test CPU | AVX2 | FMA
[   0.003003] Frontend <Info> qt_common/qt_common.cpp:267:LogSystemInfo: Host OS: Linux test system
[   0.004004] Frontend <Info> qt_common/qt_common.cpp:268:LogSystemInfo: Host RAM: 32.00 GiB
[  12.005005] Render.Vulkan <Info> video_core/renderer_vulkan/renderer_vulkan.cpp:264:Report: Driver: Test driver 1.2
[  12.006006] Render.Vulkan <Info> video_core/renderer_vulkan/renderer_vulkan.cpp:265:Report: Device: Test GPU
[  12.007007] Render.Vulkan <Info> video_core/renderer_vulkan/renderer_vulkan.cpp:266:Report: Vulkan: 1.3.0
[  12.008008] Frontend <Info> yuzu/main_window.cpp:2116:BootGame: Booting game: 0100000000000000 | Test game | 1.0.0
[  14.000000] Render.Vulkan <Critical> renderer_vulkan.cpp:99:Render: VK_ERROR_DEVICE_LOST
`;

export const validSubmission = {
  gameId: 5483943, name: 'Integration test only', handle: '', build: 'test-build', gameVersion: '1.0.0',
  cpu: 'Test CPU', gpu: 'Test GPU', driver: 'Test driver', os: 'Linux test', ram: '', graphicsApi: 'Vulkan',
  settings: 'Default settings in integration test', outcome: 'Runs', progress: 'Reached the test menu',
  summary: 'Automated integration test, not real compatibility evidence', details: 'This record only exists in the isolated test database.',
  reproduction: '', updates: '', mods: '', publicConsent: true, safeFilesConsent: true, privateLogConsent: true,
  log: { name: 'eden_log.txt', mime: 'text/plain', text: sourceLog }, website: '',
};
