/* (linux port) A build without the WebGPU renderer (gpu/render: wgpu-native is Windows only here; nx2/build.py
   defines RX_NO_WEBGPU when it is not built): its entry points as stand-ins, so the Vulkan back end is the only
   renderer and a missing one leaves the window black instead of failing the link. */
#ifdef RX_NO_WEBGPU
#include "hos.h"
#include "../gpu/render/rx_nvn.h"

int wgr_start(void *hwnd) {
    (void)hwnd;
    hos_log("rx: no Vulkan renderer, and this build has no WebGPU one: nothing is drawn");
    return 0;
}
void wgr_resize(int width, int height) { (void)width, (void)height; }
const char *wgr_describe(void) { return "none"; }
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) { *draws = *skipped = *pipelines = 0; }
void wgr_nvn_submit(const uint64_t *handles, int n) { (void)handles, (void)n; }
uint64_t wgr_nvn_fence(void) { return 0; }
uint64_t wgr_nvn_pushed(void) { return 0; }
uint64_t wgr_nvn_replayed(void) { return 0; }
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence, (void)timeout_ns; return 1; }
void wgr_wait_writebacks_reading(const void *p, uint64_t n) { (void)p, (void)n; }
void wgr_nvn_finish(void) {}
void wgr_nvn_present(const nvn_tex *t, const int crop[4]) { (void)t, (void)crop; }
void wgr_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) { (void)id, (void)t, (void)v; }
void wgr_nvn_register_sampler(int id, const nvn_samp_par *p) { (void)id, (void)p; }
void wgr_nvn_set_texture_pool_size(int n) { (void)n; }
void wgr_nvn_texture_released(const nvn_tex *t) { (void)t; }
void wgr_nvn_program_released(const nvn_prog *p) { (void)p; }
void wgr_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                         uint64_t row, uint64_t img) { (void)t, (void)level, (void)layer0, (void)r, (void)dst, (void)row, (void)img; }
void wgr_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                          const uint8_t *src, uint64_t row, uint64_t img) { (void)t, (void)level, (void)layer0, (void)r, (void)src, (void)row, (void)img; }
void wgr_nvn_invalidate(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; }

/* gpu/render's own debugging hooks and its soft write watch reader (nn_hid.c's console commands, rx_backend.c) */
uint32_t *rx_collect_written(int64_t *count) { *count = -1; return 0; }
void rx_request_frame_dump(void) {}
void rx_set_screenshot_ms(int64_t ms) { (void)ms; }
void rx_request_rdoc_capture(int frames) { (void)frames; }
void rx_flash_dark_arm(void) {}
#endif
