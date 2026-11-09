// (hookshot) NVIDIA DLSS Super Resolution in place of OoT's gameplay upscale network (vk_dlss.h).
//
// OoT has no DLSS of its own: its scene is drawn at a lower size and a network of its own (the program
// fd029a317a9a4b94 at 3/4, another for each other share; one dispatch, cooperative matrices) scales it up, from the
// square root of the scene's tonemapped colour, into the frame's B10G11R11 picture (before the HUD). Here that dispatch is replaced:
//  - the scene's camera is taken from the draws into the scene's colour target: their vertex stage's uniform buffer 3
//    holds the target's size at 0x120, this frame's view-projection at 0x30 (row major, clip = M * position; the projection's w row ends -1, 0 at 0xa8). The largest such target in a frame is
//    the scene (reflections and half-size passes have the same layout). The scene's depth is that draw's depth target.
//  - shaders/dlss_prep.comp makes DLSS's inputs at the render size: the colour as the network samples it (LDR), the
//    depth, and motion vectors from the depth reprojected through the two cameras. The game has no motion vectors of its
//    own (no velocity target: the scene pass draws into one B10G11R11 target, and its upscale network samples the colour alone),
//    so that is the camera's motion, and what moves by itself would ghost. The scene camera's sub-pixel jitter
//    (nx2/oot_jitter.py, while DLSS draws) is read back from its projection, given to DLSS and taken off the motion.
//  - actor motion (actor_motion, shaders/dlss_actor.vert): the skinned draws into the scene (vk_draw.cpp's dlss_skin_check:
//    Link, the other characters, the animals, what the game animates with bones) are drawn again over that motion, each
//    vertex skinned with this frame's bones and the frame before's, so their pixels get their own motion. Their poses are
//    copied on the GPU from where the game has them into two buffers that take turns; a pixel is the draw's where its depth is
//    the scene's (within RX_VK_DLSS_ACTOR_TOL steps, default 64). What moves without bones (particles, water, foliage moved in
//    its vertex shader, mesh-shader draws) keeps the camera's motion. RX_VK_DLSS_ACTORS=0: camera motion alone.
//  - DLSS (NGX, Vulkan) scales them to the output's size; shaders/dlss_out.comp squares that back to linear colour
//    into the network's output.
// Anything unexpected (no camera this frame, other sizes, DLSS not there or failing) leaves the network to run, the
// reason in the log when it changes.
//
// RX_VK_DLSS=0 / 1 / 2: the game's own, DLSS, DLAA over the Insert menu's Upscaler row (0 also leaves NGX's extensions
// off). DLSS's runtime (Windows: nvngx_dlss.dll; Linux: libnvidia-ngx-dlss.so.*) is looked for in RX_VK_DLSS_DIR=<folder>
// when set, the game folder's ngx folder, and %LOCALAPPDATA%\Hookshot\ngx (Linux: $XDG_DATA_HOME/hookshot/ngx): NGX takes
// the newest one it finds in them. Windows' paths are wide all the way (GetModuleFileNameW, _wgetenv), so any folder name
// works. The Insert menu's DLSS model row (opts.dlss_model, upscaler_rows.h) chooses DLSS's preset: K by default, M, L, or
// NVIDIA's default (0). RX_VK_DLSS_PRESET=<letter> (or 0) over it.
// RX_VK_DLSS_DEBUG=motion: the motion vectors shown instead of the picture (|x|, |y| over 16 px as red and green, the actors'
// in it); =alternate: every other frame the game's own upscale (a test's reference beside DLSS's). RX_VK_DLSS_LOG=1: NGX's
// own log in ours.
// RX_VK_DLSS_SQUARE=0: the output not squared.
//
// NVIDIA's NGX SDK is not part of Hookshot (NVIDIA's license): a local build fetches it (scripts/ngx_sdk.py) and builds with
// HOOKSHOT_NGX_SDK, which compiles this file with HOOKSHOT_NGX. Without it only the stubs at the end are here.
// NGX's core is the driver's (Windows: _nvngx.dll, which NVIDIA's loader finds in the driver's own folder; Linux:
// libnvidia-ngx.so). It is never put beside the game: a copy there would be loaded in place of the driver's.
#include "vk_dlss.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "dlss_actor.h"
#include "dlss_math.h"
#include "upscaler_rows.h"
#include "vk_draw.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_texture.h"

#ifdef HOOKSHOT_NGX
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"

#include "shaders/dlss_actor_frag_spv.h"
#include "shaders/dlss_actor_vert_spv.h"
#include "shaders/dlss_out_comp_spv.h"
#include "shaders/dlss_prep_comp_spv.h"
#endif
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
}  // namespace

// (what is known of DLSS on this device: -1 not yet, else a DlssWhy. Set once the device is made and when NGX answers)
std::atomic<int> g_why{-1};

DlssWhy dlss_why() {
    if (!dlss_built()) return DLSS_NOT_BUILT;
    static const bool env_off = env_switch("RX_VK_DLSS") == 0;
    if (env_off) return DLSS_ENV_OFF;
    const int w = g_why.load(std::memory_order_relaxed);
    return w < 0 ? DLSS_OK : (DlssWhy)w;
}

int dlss_mode() {
    if (dlss_why() != DLSS_OK) return 0;
    static const int env = env_switch("RX_VK_DLSS");
    return std::clamp(env >= 0 ? env : opts.dlss, 0, 2);
}
bool dlss_wanted() { return dlss_mode() != 0; }
bool dlss_full_scale() { return dlss_mode() == 2; }

#ifdef HOOKSHOT_NGX

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

struct Camera {
    float current[16]{};      // (its view-projection)
    float jitter[2]{};        // (the projection's offset of the picture's centre, in clip space: nx2/oot_jitter.py)
    uint32_t width = 0, height = 0;
    Texture* depth = nullptr;
    uint64_t depth_gen = 0;   // (texture_destroy_generation when taken: a depth texture destroyed since is not used)
    uint64_t frame = 0;       // (texture_frame_no when taken)
    bool valid = false;
};

struct State {
    std::mutex mu;
    bool ngx_tried = false, ngx_ok = false;
    std::atomic<bool> failed{false};   // (atomic: dlss_jitter_size reads it on the game's thread, without the lock)
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t feature_rw = 0, feature_rh = 0, feature_ow = 0, feature_oh = 0;   // (its largest render size, its output)
    uint32_t min_rw = 0, min_rh = 0;
    unsigned feature_preset = ~0u;   // (the preset it was made with: another one from the menu makes it again)
    uint64_t features_made = 0;      // (how many times: a feature made again in play costs a stall, so each says why)
    Image color, depth, motion, output;
    VkDescriptorSetLayout prep_set = VK_NULL_HANDLE, out_set = VK_NULL_HANDLE;
    VkPipelineLayout prep_layout = VK_NULL_HANDLE, out_layout = VK_NULL_HANDLE;
    VkPipeline prep = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    Camera cam;               // (this frame's, since the last upscale)
    std::atomic<uint64_t> last_frame{0};   // (the frame DLSS last drew: read on the game's thread too)
    float last_jitter[2]{};   // (its camera's jitter: the frame before's, for the next one)
    float last_vp[16]{};      // (its camera's view-projection: the frame before's)
    std::atomic<uint32_t> jitter_w{0}, jitter_h{0};   // (the scene's size while DLSS draws: the host's jitter's scale)
    std::atomic<uint32_t> jitter_phases{8};           // (its sequence's length, for DLSS's scale: dlss_jitter_phases)
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    const char* reason = nullptr;
    int reason_repeats = 0;
    bool told_first = false;
    // (actor motion: the skinned draws into the scene since the last upscale; the poses DLSS last drew, by draw: the
    // frame before's, where they were copied to in one of the two pose buffers (each frame's poses go to the other one))
    struct Skin {
        DlssSkin k;
        uint64_t key = 0;
        uint32_t nbones = 0;
    };
    std::vector<Skin> skins;
    struct Pose {
        uint32_t at = 0, nbones = 0;   // (bytes into its buffer: the origin, then the bones from at + 16)
        uint64_t frame = 0;
    };
    std::unordered_map<uint64_t, Pose> poses;
    std::unordered_map<uint64_t, uint32_t> model_bones;   // (by model: the bones its vertices use, 0 when unreadable)
    struct PoseBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        uint64_t address = 0;
    } pose_buffers[2];
    int pose_at = 0;            // (this frame's)
    VkPipelineLayout actor_layout = VK_NULL_HANDLE;
    VkPipeline actor = VK_NULL_HANDLE;
    bool actor_tried = false;
    struct Upload {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* map = nullptr;
    } uploads[6];               // (the cameras: a ring, one in use stays so for a few frames, as what later() frees)
    int upload_at = 0;
    uint64_t actor_draws = 0, actor_frames = 0, actor_skipped = 0;
    int64_t actor_ns = 0;       // (the CPU's time in it, since the last status line)
    uint64_t status_draws = 0, status_frames = 0;   // (actor_draws and _frames at the last status line: dlss_status)
    uint64_t status_drawn = 0, status_passed = 0;   // (since the last status line: frames DLSS drew, and frames a network
                                                    // of the upscale's shape ran without it (its scene not this frame's))
    float status_jitter[2]{};   // (the last evaluation's jitter, in render pixels, and whether it had motion)
    bool status_motion = false;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
    uint64_t asked = 0;       // (the frame an upscale last asked for DLSS: dlss_idle)
    std::atomic<bool> holding{false};   // (a feature, images or frees still to do: dlss_idle has something to let go)
};
State& st() {
    static State s;
    return s;
}

bool fail(const char* why) {   // (the network runs: why, when it changes and every 600th time)
    State& s = st();
    if (why != s.reason || ++s.reason_repeats % 600 == 0) logf("vk: NVIDIA DLSS: the game's upscale runs (%s)", why);
    if (why != s.reason) s.reason = why, s.reason_repeats = 0;
    return false;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead(bool all) {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (all || now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}

void drop_image(Image& im) {
    if (!im.image) return;
    const Image old = im;
    later([old] {
        vkDestroyImageView(dev.device, old.view, nullptr);
        destroy_image(old.image, old.memory);
    });
    im = {};
}

bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags extra = 0) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    drop_image(im);
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
    if (!create_image(ic, im.image, im.memory)) return im = {}, false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.memory);
        return im = {}, false;
    }
    im.format = format, im.width = w, im.height = h;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

NVSDK_NGX_Resource_VK resource(const Image& im, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, im.format,
                                                  im.width, im.height, write);
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m(message ? message : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    logf("ngx: %s", m.c_str());
}

namespace fs = std::filesystem;
#ifdef _WIN32
/// The game folder (game_module_path's rule: bin\hookshot.exe beside image.bin is the folder above bin), from
/// GetModuleFileNameW: a folder with characters outside the ANSI code page is still the right one
fs::path game_dir() {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (!n || n >= exe.size()) return {};
    exe.resize(n);
    fs::path dir = fs::path(exe).parent_path();
    if (!_wcsicmp(dir.filename().c_str(), L"bin") && GetFileAttributesW((dir.parent_path() / L"image.bin").c_str()) != INVALID_FILE_ATTRIBUTES)
        dir = dir.parent_path();
    return dir;
}
/// An environment variable as a path (_wgetenv: not through the ANSI code page); empty when unset
fs::path env_path(const wchar_t* name) {
    const wchar_t* e = _wgetenv(name);
    return e && *e ? fs::path(e) : fs::path();
}
/// A path for our log (UTF-8)
std::string shown(const fs::path& p) {
    const std::wstring& w = p.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
/// A path for NGX, which takes wide strings: Windows' own
std::wstring wide(const fs::path& p) { return p.native(); }
#else
fs::path game_dir() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    return fs::path(std::string(exe, n)).parent_path();
}
fs::path env_path(const char* name) {
    const char* e = getenv(name);
    return e && *e ? fs::path(e) : fs::path();
}
std::string shown(const fs::path& p) { return p.native(); }
std::wstring wide(const fs::path& p) {
    const std::string& s = p.native();
    return std::wstring(s.begin(), s.end());
}
#endif

#ifdef _WIN32
constexpr const char* kRuntime = "nvngx_dlss.dll";   // (DLSS's runtime, as scripts/ngx_sdk.py fetches it)
constexpr const fs::path::value_type* kRuntimeName = L"nvngx_dlss.dll";
#else
constexpr const char* kRuntime = "libnvidia-ngx-dlss.so";
constexpr const fs::path::value_type* kRuntimeName = "libnvidia-ngx-dlss.so";
#endif

/// NGX on this device, once: false when DLSS cannot run here (the reason logged)
bool ngx_init() {
    State& s = st();
    if (s.ngx_tried) return s.ngx_ok;
    s.ngx_tried = true;
    // where DLSS's runtime (kRuntime) is looked for (NGX takes the newest it finds), and its own files go
    std::vector<fs::path> dirs;
#ifdef _WIN32
    if (fs::path e = env_path(L"RX_VK_DLSS_DIR"); !e.empty()) dirs.push_back(e);
    dirs.push_back(game_dir() / L"ngx");
    if (fs::path l = env_path(L"LOCALAPPDATA"); !l.empty()) dirs.push_back(l / L"Hookshot" / L"ngx");
#else
    if (fs::path e = env_path("RX_VK_DLSS_DIR"); !e.empty()) dirs.push_back(e);
    dirs.push_back(game_dir() / "ngx");
    if (fs::path x = env_path("XDG_DATA_HOME"); !x.empty()) dirs.push_back(x / "hookshot" / "ngx");
    else if (fs::path h = env_path("HOME"); !h.empty()) dirs.push_back(h / ".local" / "share" / "hookshot" / "ngx");
#endif
    std::vector<std::wstring> wdirs;
    for (const auto& d : dirs) wdirs.push_back(wide(d));
    std::vector<const wchar_t*> paths;
    for (const auto& d : wdirs) paths.push_back(d.c_str());
    const fs::path data = game_dir() / "ngx";
    std::error_code made;   // (not the throwing overload: a folder that cannot be made, read-only or so, must not end the game)
    fs::create_directories(data, made);   // (NGX's logs and its own files: the game folder's, not /usr/share)
    const std::wstring wdata = wide(data);
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths.data();
    info.PathListInfo.Length = (unsigned)paths.size();
    static const bool verbose = env_switch("RX_VK_DLSS_LOG") > 0;
    info.LoggingInfo.LoggingCallback = verbose ? ngx_log : nullptr;
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    // (NGX also writes its own log files into the data folder; with RX_VK_DLSS_LOG=1 its lines go to ours instead)
    info.LoggingInfo.DisableOtherLoggingSinks = verbose;
    // (a project id of Hookshot's own: NGX wants one with the custom engine type, any GUID)
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID("7b2f5d0c-6a1e-4c39-9f84-3d5e2a1b0c96", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              "1.0", wdata.c_str(), dev.instance, dev.physical, dev.device,
                                                              instance_proc(), vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: NGX did not start (%#x): not an NVIDIA GPU, or its driver has no NGX", (unsigned)r);
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.caps)) || !s.caps) {
        logf("vk: NVIDIA DLSS: NGX has no capabilities for this device");
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    // (NGX's C functions, not NVSDK_NGX_Parameter's virtual ones: its object comes from NVIDIA's MSVC-built library, whose
    // vtable order is not the Itanium ABI's that llvm-mingw calls with. The overloads only lined up by luck)
    int available = 0, needs_driver = 0;
    unsigned major = 0, minor = 0;
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    NVSDK_NGX_Parameter_GetUI(s.caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    std::string where;
    for (const auto& d : dirs) where += (where.empty() ? "" : ", ") + shown(d);
    if (!available) {
        // (for the menu: a runtime found in none of the folders is the likely reason; with one there, the GPU (a GTX))
        int init = 0;
        NVSDK_NGX_Parameter_GetI(s.caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init);
        bool runtime = false;
        std::error_code ec;
        for (const auto& d : dirs)
            for (const auto& f : fs::directory_iterator(d, ec))
                runtime |= f.path().filename().native().rfind(kRuntimeName, 0) == 0;
        if (needs_driver) logf("vk: NVIDIA DLSS: needs NVIDIA's driver %u.%u or newer", major, minor);
        else if (!runtime) logf("vk: NVIDIA DLSS: not available: no %s in %s (scripts/ngx_sdk.py fetches it)", kRuntime, where.c_str());
        else logf("vk: NVIDIA DLSS: not available on this GPU (%#x): DLSS needs an NVIDIA RTX GPU", (unsigned)init);
        g_why = needs_driver ? DLSS_NEEDS_DRIVER : !runtime ? DLSS_NO_RUNTIME : DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&s.params)) || !s.params) return false;
    logf("vk: NVIDIA DLSS: NGX started (DLSS's runtime: the newest %s in %s)", kRuntime, where.c_str());
    s.ngx_ok = true;
    g_why = DLSS_OK;
    return true;
}

/// One compute pipeline with push descriptors; what it made is destroyed again when a step fails
bool make_pipeline(const uint32_t* code, size_t size, const VkDescriptorType* types, uint32_t n, uint32_t push,
                   VkDescriptorSetLayout& set, VkPipelineLayout& layout, VkPipeline& pipe) {
    const auto undo = [&] {
        if (layout) vkDestroyPipelineLayout(dev.device, layout, nullptr);
        if (set) vkDestroyDescriptorSetLayout(dev.device, set, nullptr);
        set = VK_NULL_HANDLE, layout = VK_NULL_HANDLE, pipe = VK_NULL_HANDLE;
        return false;
    };
    VkDescriptorSetLayoutBinding b[8]{};
    for (uint32_t i = 0; i < n; i++) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = n, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return set = VK_NULL_HANDLE, undo();
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout) != VK_SUCCESS) return layout = VK_NULL_HANDLE, undo();
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size, mi.pCode = code;
    VkShaderModule m{};
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return undo();
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = layout;
    const bool ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS;
    vkDestroyShaderModule(dev.device, m, nullptr);
    return ok || undo();
}

/// The two passes' pipelines and the samplers, once (dlss_upscale stops trying when this fails)
bool make_pipelines() {
    State& s = st();
    if (s.prep && s.out && s.point && s.linear) return true;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType prep_types[5] = {S, S, I, I, I}, out_types[2] = {S, I};
    if (!s.prep && !make_pipeline(DLSS_PREP_COMP_SPV, sizeof DLSS_PREP_COMP_SPV, prep_types, 5, 84, s.prep_set, s.prep_layout, s.prep))
        return false;
    if (!s.out && !make_pipeline(DLSS_OUT_COMP_SPV, sizeof DLSS_OUT_COMP_SPV, out_types, 2, 8, s.out_set, s.out_layout, s.out))
        return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!s.point && vkCreateSampler(dev.device, &si, nullptr, &s.point) != VK_SUCCESS) return s.point = VK_NULL_HANDLE, false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (!s.linear && vkCreateSampler(dev.device, &si, nullptr, &s.linear) != VK_SUCCESS) return s.linear = VK_NULL_HANDLE, false;
    return true;
}

/// A view of a depth texture's depth alone, for sampling (made once per image)
VkImageView depth_view(Texture* t) {
    State& s = st();
    const uint64_t gen = texture_destroy_generation();
    if (gen != s.depth_views_gen) {   // (a texture was destroyed: its image's view may be stale)
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
        s.depth_views_gen = gen;
    }
    if (auto it = s.depth_views.find(t->image); it != s.depth_views.end()) return it->second;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t->image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = t->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vc, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    s.depth_views[t->image] = v;
    return v;
}

// ---- actor motion: the skinned draws drawn again into the motion, each pixel they show moved by its own bones ----

using ActorPush = dlss_actor::Push;

/// Its pipeline, once (false: none, the motion stays the camera's)
bool make_actor_pipeline() {
    State& s = st();
    if (s.actor) return true;
    if (s.actor_tried) return false;
    s.actor_tried = true;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    if (!dev.vk12.bufferDeviceAddress || !f.features.shaderInt64) {   // (its shaders read the game's buffers by their addresses)
        logf("vk: NVIDIA DLSS: no buffer device addresses or 64-bit integers in shaders: camera motion alone");
        return false;
    }
    VkDescriptorSetLayoutBinding b[2]{};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2, li.pBindings = b;
    VkDescriptorSetLayout set{};
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ActorPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &s.actor_layout) != VK_SUCCESS) return false;
    VkShaderModule m[2]{};
    const uint32_t* code[2] = {DLSS_ACTOR_VERT_SPV, DLSS_ACTOR_FRAG_SPV};
    const size_t size[2] = {sizeof DLSS_ACTOR_VERT_SPV, sizeof DLSS_ACTOR_FRAG_SPV};
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size[i], mi.pCode = code[i];
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m[i]) != VK_SUCCESS) return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = m[i], stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2, ds.pDynamicStates = dyn;
    const VkFormat format = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = 2, gi.pStages = stages;
    gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia, gi.pViewportState = &vp, gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms, gi.pColorBlendState = &cb, gi.pDynamicState = &ds, gi.layout = s.actor_layout;
    const bool ok = vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &gi, nullptr, &s.actor) == VK_SUCCESS;
    for (VkShaderModule x : m) vkDestroyShaderModule(dev.device, x, nullptr);
    if (!ok) s.actor = VK_NULL_HANDLE, logf("vk: NVIDIA DLSS: the actor motion's pipeline was not made: camera motion alone");
    return ok;
}

/// A range of guest memory as this recording binds it, by its device address (the pools and their mirrors have them)
uint64_t device_address(const BufferRef& r) {
    if (!r) return 0;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = r.buffer;
    const VkDeviceAddress base = vkGetBufferDeviceAddress(dev.device, &da);
    return base ? base + r.offset : 0;
}

constexpr uint64_t kPoseBytes = 4 << 20;   // (a pose buffer: 280 bones of 48 bytes are 13 KB a draw, 300 of them)

/// The skinned draws into the scene (cam's depth) with a pose of the frame before (DLSS's last), drawn into the motion
/// image over the camera's motion. Each one's bones and origin are copied on the GPU, from where the game has them, into
/// this frame's pose buffer, for this frame and the next. before: the frame before's camera without its jitter. history:
/// DLSS has the frame before. Returns the draws drawn
int actor_motion(VkCommandBuffer cmd, const Camera& cam, const float* before, uint32_t rw, uint32_t rh, VkImageView dview,
                 bool history) {
    State& s = st();
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    std::vector<State::Skin> skins = std::move(s.skins);
    s.skins.clear();
    std::unordered_map<uint64_t, State::Pose> old = std::move(s.poses);
    s.poses.clear();
    static const bool off = env_switch("RX_VK_DLSS_ACTORS") == 0;
    if (off || skins.empty() || !make_actor_pipeline()) return 0;
    for (auto& b : s.pose_buffers)
        if (!b.buffer) {
            if (!create_device_buffer(kPoseBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      b.buffer, b.memory))
                return b = {}, 0;
            b.address = device_address({b.buffer, 0, kPoseBytes, 0});
        }
    const uint64_t now = texture_frame_no();
    const State::PoseBuffer& here = s.pose_buffers[s.pose_at];
    const State::PoseBuffer& there = s.pose_buffers[s.pose_at ^ 1];
    static const uint32_t tolerance = env_switch("RX_VK_DLSS_ACTOR_TOL") >= 0 ? (uint32_t)env_switch("RX_VK_DLSS_ACTOR_TOL") : 64;
    std::vector<ActorPush> draws;
    std::vector<uint32_t> counts;
    uint32_t at = 0;
    for (const auto& k : skins) {
        if (k.k.depth != cam.depth) continue;
        const uint32_t bytes = 16 + k.nbones * 48;
        if (at + bytes > kPoseBytes) break;
        // (this frame's pose, from where the game has it: its bones, then its origin at the object's 0x40)
        const BufferRef bones = gpu_buffer_bind(k.k.bones, (uint64_t)k.nbones * 48, false);
        const BufferRef origin = gpu_buffer_bind(k.k.object + 0x40, 12, false);
        if (!bones || !origin) {
            s.actor_skipped++;
            continue;
        }
        const VkBufferCopy c0{origin.offset, at, 12}, c1{bones.offset, at + 16, (uint64_t)k.nbones * 48};
        vkCmdCopyBuffer(cmd, origin.buffer, here.buffer, 1, &c0);
        vkCmdCopyBuffer(cmd, bones.buffer, here.buffer, 1, &c1);
        s.poses[k.key] = {at, k.nbones, now};
        const uint32_t mine = at;
        at += (bytes + 15) & ~15u;
        const auto it = old.find(k.key);
        if (!history || it == old.end() || it->second.frame != s.last_frame || it->second.nbones != k.nbones) {
            s.actor_skipped++;
            continue;
        }
        // (the vertices and indices where the game has them: its buffers as this recording binds them)
        const uint64_t va = device_address(gpu_buffer_bind(k.k.vbuf, k.k.vsize, false));
        const uint64_t ia = device_address(gpu_buffer_bind(k.k.ibuf, (uint64_t)k.k.count << k.k.itype, false));
        if (!va || !ia || va % 4 || k.k.count <= 0 || k.k.itype < 0 || k.k.itype > 2) {
            s.actor_skipped++;
            continue;
        }
        ActorPush a{};
        a.vertices = va, a.count = (uint32_t)std::min<uint64_t>(k.k.vsize / k.k.stride, UINT32_MAX);
        dlss_actor::index_start(ia, k.k.itype, &a.indices, &a.first);
        a.origins[0] = here.address + mine, a.origins[1] = there.address + it->second.at;
        a.bones[0] = a.origins[0] + 16, a.bones[1] = a.origins[1] + 16;
        a.nbones = k.nbones;
        a.itype = (uint32_t)k.k.itype, a.base_vertex = k.k.base_vertex;
        a.stride = k.k.stride / 4, a.pos = k.k.pos, a.sets = (uint32_t)k.k.sets;
        for (int i = 0; i < 2; i++) a.joints[i] = k.k.joints[i], a.weights[i] = k.k.weights[i];
        a.formats = k.k.formats;
        a.tolerance = tolerance;
        draws.push_back(a);
        counts.push_back((uint32_t)k.k.count);
    }
    s.pose_at ^= 1;
    if (draws.empty()) return 0;
    // (the cameras: this frame's as drawn, the frame before's without its jitter, the jitter, the render size)
    State::Upload& u = s.uploads[s.upload_at];
    s.upload_at = (s.upload_at + 1) % (int)(sizeof s.uploads / sizeof s.uploads[0]);
    if (!u.buffer && !create_buffer_cached(dlss_actor::kHeadWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, u.buffer, u.memory, &u.map))
        return u = {}, 0;
    float* head = (float*)u.map;
    std::memcpy(head, cam.current, 64);
    std::memcpy(head + 16, before, 64);
    head[32] = cam.jitter[0], head[33] = cam.jitter[1], head[34] = (float)rw, head[35] = (float)rh;
    flush_mapped(u.memory, dlss_actor::kHeadWords * 4);
    full_barrier(cmd);   // (the poses copied)
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = s.motion.view, ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rw, rh}}, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.actor);
    const VkViewport viewport{0, 0, (float)rw, (float)rh, 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {rw, rh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkDescriptorBufferInfo bi{u.buffer, 0, dlss_actor::kHeadWords * 4};
    const VkDescriptorImageInfo ii{s.point, dview, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[0].pBufferInfo = &bi;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &ii;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.actor_layout, 0, 2, w);
    for (size_t i = 0; i < draws.size(); i++) {   // (vertex i of a draw is its i-th index)
        vkCmdPushConstants(cmd, s.actor_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof draws[i], &draws[i]);
        vkCmdDraw(cmd, counts[i], 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    s.actor_draws += draws.size(), s.actor_frames++;
    return (int)draws.size();
}

/// DLSS's preset: the DLSS model row's, RX_VK_DLSS_PRESET over it (upscaler_rows.h dlss_preset)
unsigned wanted_preset() {
    static const char* const env = getenv("RX_VK_DLSS_PRESET");
    return upscaler_rows::dlss_preset(opts.dlss_model, env && *env ? env : nullptr);
}

/// DLSS's feature for this output size and preset, made when either changes (cmd: its creation's work)
bool make_feature(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    State& s = st();
    const unsigned preset = wanted_preset();
    if (s.feature && s.feature_ow == ow && s.feature_oh == oh && rw <= s.feature_rw && rh <= s.feature_rh &&
        rw >= s.min_rw && rh >= s.min_rh && s.feature_preset == preset)
        return true;
    // (why it is made: in the log with its time, so a stall in play can be matched with one)
    char why[160];
    if (!s.feature && !s.features_made) snprintf(why, sizeof why, "the first");
    else if (!s.feature) snprintf(why, sizeof why, "again after it was let go");
    else if (s.feature_ow != ow || s.feature_oh != oh)
        snprintf(why, sizeof why, "the output went from %ux%u to %ux%u", s.feature_ow, s.feature_oh, ow, oh);
    else if (s.feature_preset != preset)
        snprintf(why, sizeof why, "preset %c, was %c (the DLSS model row)", upscaler_rows::preset_letter(preset),
                 upscaler_rows::preset_letter(s.feature_preset));
    else
        snprintf(why, sizeof why, "the render size %ux%u left its range, %ux%u to %ux%u", rw, rh, s.min_rw, s.min_rh,
                 s.feature_rw, s.feature_rh);
    if (s.feature) {
        NVSDK_NGX_Handle* old = s.feature;
        later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
        s.feature = nullptr;
    }
    // (the quality mode whose range of render sizes holds this one: the network's 3/4 is DLSS's MaxQuality range)
    // (DLAA's when the render size is the output's)
    static const NVSDK_NGX_PerfQuality_Value scaled[] = {NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_DLAA,
                                                        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
                                                        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    static const NVSDK_NGX_PerfQuality_Value same[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality};
    const bool aa = rw == ow && rh == oh;
    const NVSDK_NGX_PerfQuality_Value* modes = aa ? same : scaled;
    const size_t nmodes = aa ? 2 : 5;
    NVSDK_NGX_PerfQuality_Value mode = modes[0];
    unsigned max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    bool found = aa;
    if (aa) max_w = min_w = ow, max_h = min_h = oh;   // (DLAA: the render size is the output's, one size)
    for (size_t i = 0; i < nmodes && !found; i++) {
        const NVSDK_NGX_PerfQuality_Value m = modes[i];
        unsigned w = 0, h = 0, xw = 0, xh = 0, nw = 0, nh = 0;
        float sharp = 0;
        if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(s.caps, ow, oh, m, &w, &h, &xw, &xh, &nw, &nh, &sharp)) || !w) continue;
        if (rw >= nw && rh >= nh && rw <= xw && rh <= xh) {
            mode = m, max_w = xw, max_h = xh, min_w = nw, min_h = nh, found = true;
            break;
        }
    }
    if (!found) return fail("no DLSS mode takes this render size");
    // (the DLSS model row's preset for every mode, K by default: NVIDIA's transformer model. 0: NVIDIA's own per mode)
    static_assert(NVSDK_NGX_DLSS_Hint_Render_Preset_K == 11 && NVSDK_NGX_DLSS_Hint_Render_Preset_L == 12 &&
                  NVSDK_NGX_DLSS_Hint_Render_Preset_M == 13 && NVSDK_NGX_DLSS_Hint_Render_Preset_Default == 0);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(s.params, k, preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = max_w, cp.Feature.InHeight = max_h, cp.Feature.InTargetWidth = ow, cp.Feature.InTargetHeight = oh;
    cp.Feature.InPerfQualityValue = mode;
    // (LDR colour, already tonemapped: no exposure; motion at the render size, not jittered; depth not reversed)
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    const auto t0 = std::chrono::steady_clock::now();
    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &s.feature, s.params, &cp);
    const double made_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    note_work();   // (NGX's own commands, which vk_work.h does not see: the next full_barrier is not left out)
    if (NVSDK_NGX_FAILED(r) || !s.feature) {
        logf("vk: NVIDIA DLSS: its feature was not made (%#x) for %ux%u -> %ux%u", (unsigned)r, max_w, max_h, ow, oh);
        s.feature = nullptr;
        s.failed = true;
        return false;
    }
    s.feature_rw = max_w, s.feature_rh = max_h, s.feature_ow = ow, s.feature_oh = oh, s.min_rw = min_w, s.min_rh = min_h;
    s.feature_preset = preset;
    s.features_made++;
    logf("vk: NVIDIA DLSS: feature %llu made (%s; frame %llu, %.1f ms of CPU): %ux%u (%ux%u to %ux%u) -> %ux%u, mode %d, "
         "preset %c%s", (unsigned long long)s.features_made, why, (unsigned long long)texture_frame_no(), made_ms, rw, rh,
         min_w, min_h, max_w, max_h, ow, oh, (int)mode, upscaler_rows::preset_letter(preset),
         preset ? "" : " (NVIDIA's default)");
    s.last_frame = 0;   // (a new history)
    return true;
}

}  // namespace

bool dlss_built() { return true; }

void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (env_switch("RX_VK_DLSS") == 0) return;
    for (const auto& e : all)
        if (!strcmp(e.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            want.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            return;
        }
}

void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    // (NGX's: VK_NVX_binary_import and VK_NVX_image_view_handle, with push descriptors and buffer device addresses,
    // which the replay enables already: core 1.2's bufferDeviceAddress stands for VK_EXT_buffer_device_address. The NVX ones
    // are on whenever the device has them, so the Insert menu's row needs no restart)
    if (env_switch("RX_VK_DLSS") == 0) return;
    bool both = true;
    for (const char* name : {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME}) {
        bool have = false, listed = false;
        for (const auto& e : all) have |= !strcmp(e.extensionName, name);
        for (const char* w : want) listed |= !strcmp(w, name);
        if (have && !listed) want.push_back(name);
        both &= have;
    }
    // (NVIDIA's own extensions: without them this is not an NVIDIA GPU, or not one NGX runs on. With them, NGX says, in
    // dlss_probe or at the first upscale: a GTX has them too)
    if (!both) g_why = DLSS_NEEDS_RTX;
}

void dlss_probe() {
    if (dlss_why() != DLSS_OK || g_why.load(std::memory_order_relaxed) >= 0 || !dev.device) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    ngx_init();
}

void dlss_note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                          uint64_t camera_size) {
    if (!color0 || !depth || !camera || camera_size < 0x230 || width < 640 || height < 360) return;
    if (color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !dlss_wanted()) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (s.cam.valid && s.cam.frame == now && (uint64_t)width * height <= (uint64_t)s.cam.width * s.cam.height)
        return;   // (the largest one of the frame wins)
    float c[0x230 / 4];
    if (!memory_read_as_submitted(camera, c, sizeof c)) return;
    // (the target's size at 0x120; the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0)
    if (c[0x120 / 4] != (float)width || c[0x124 / 4] != (float)height || c[0xa0 / 4] != 0.0f || c[0xa4 / 4] != 0.0f ||
        c[0xa8 / 4] != -1.0f || c[0xac / 4] != 0.0f)
        return;
    Camera& k = s.cam;
    std::memcpy(k.current, c + 0x30 / 4, sizeof k.current);
    // (the offset sead's projection gave the frustum, in its third column: 2 * offset, which moves the picture by -that)
    k.jitter[0] = -c[0x78 / 4], k.jitter[1] = -c[0x88 / 4];
    k.width = width, k.height = height, k.depth = depth, k.depth_gen = texture_destroy_generation(), k.frame = now;
    k.valid = true;
}

uint32_t dlss_jitter_size(int axis) {
    // (RX_VK_DLSS_JITTER=0: none. 0 too once DLSS has not drawn for a while: the camera as the game has it)
    static const bool off = env_switch("RX_VK_DLSS_JITTER") == 0;
    State& s = st();
    const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
    if (off || !dlss_wanted() || s.failed.load(std::memory_order_relaxed) || !last || texture_frame_no() - last > 30) return 0;
    if (axis == 2) return s.jitter_phases.load(std::memory_order_relaxed);
    return axis ? s.jitter_h.load(std::memory_order_relaxed) : s.jitter_w.load(std::memory_order_relaxed);
}

int dlss_status(char* b, size_t n) {
    State& s = st();
    const uint64_t last = s.last_frame.load(std::memory_order_relaxed);
    const int mode = dlss_mode();
    if (!mode || s.failed.load(std::memory_order_relaxed) || !last || texture_frame_no() - last > 30) return 0;
    std::lock_guard lk(s.mu);
    // (its own video memory: the images and buffers it made here, and what NGX says its feature holds, when it says)
    uint64_t own = 0;
    for (const Image* im : {&s.color, &s.depth, &s.motion, &s.output})
        if (im->image) own += allocation_size(im->memory);
    for (const auto& pb : s.pose_buffers)
        if (pb.buffer) own += allocation_size(pb.memory);
    for (const auto& u : s.uploads)
        if (u.buffer) own += allocation_size(u.memory);
    unsigned long long ngx = 0;
    const bool ngx_known = s.caps && NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(s.caps, &ngx));
    const uint64_t draws = s.actor_draws - s.status_draws, frames = s.actor_frames - s.status_frames;
    char feature[48] = "";
    if (ngx_known) snprintf(feature, sizeof feature, ", its feature %llu MB", ngx >> 20);
    char passed[64] = "";
    if (s.status_passed) snprintf(passed, sizeof passed, ", %llu left to the game's network", (unsigned long long)s.status_passed);
    snprintf(b, n, "; %s: %llu frames drawn%s, %llu features made, jitter %+.3f %+.3f px%s, actor motion %llu draws in %llu "
             "frames (%.3f ms CPU a frame), its images %llu MB%s",
             mode == 2 ? "DLAA" : "DLSS", (unsigned long long)s.status_drawn, passed, (unsigned long long)s.features_made,
             s.status_jitter[0], s.status_jitter[1], s.status_motion ? "" : " (no motion)",
             (unsigned long long)draws, (unsigned long long)frames, frames ? s.actor_ns / 1e6 / (double)frames : 0.0,
             (unsigned long long)(own >> 20), feature);
    s.status_draws = s.actor_draws, s.status_frames = s.actor_frames, s.actor_ns = 0, s.status_drawn = s.status_passed = 0;
    return mode;
}

void dlss_idle() {
    State& s = st();
    if (!s.holding.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (now - s.asked <= 120) return;   // (DLSS in use, or off for under 120 frames (2 s): kept, a row stepped through comes back at once)
    if (s.feature) {
        NVSDK_NGX_Handle* old = s.feature;
        later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
        s.feature = nullptr;
        s.feature_rw = s.feature_rh = s.feature_ow = s.feature_oh = s.min_rw = s.min_rh = 0;
        logf("vk: NVIDIA DLSS: not used for 120 frames (the last at frame %llu, now %llu): its feature and images let go; "
             "the next upscale with it makes them again", (unsigned long long)s.asked, (unsigned long long)now);
    }
    for (Image* im : {&s.color, &s.depth, &s.motion, &s.output}) drop_image(*im);
    for (auto& pb : s.pose_buffers)   // (the actor motion's poses and cameras)
        if (pb.buffer) {
            const State::PoseBuffer gone = pb;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            pb = {};
        }
    for (auto& u : s.uploads)
        if (u.buffer) {
            const State::Upload gone = u;
            later([gone] { destroy_buffer(gone.buffer, gone.memory); });
            u = {};
        }
    s.poses.clear(), s.skins.clear(), s.model_bones.clear();
    if (!s.depth_views.empty()) {
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
    }
    s.last_frame = 0;   // (a new history when it comes back)
    free_dead(false);
    s.holding = !s.dead.empty();
}

namespace {
/// The bones a skinned draw's vertices use (the largest joint with a weight, plus one), read once a model; 0 when its
/// buffers cannot be read
uint32_t bones_used(const DlssSkin& k) {
    if (k.count <= 0 || k.count > (1 << 20) || k.itype < 0 || k.itype > 2 || !k.stride) return 0;
    std::vector<uint8_t> idx((size_t)k.count << k.itype);
    if (!memory_read_as_submitted(k.ibuf, idx.data(), idx.size())) return 0;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < k.count; i++) {
        uint32_t v = 0;
        std::memcpy(&v, idx.data() + ((size_t)i << k.itype), (size_t)1 << k.itype);
        lo = std::min(lo, v), hi = std::max(hi, v);
    }
    const int64_t first = (int64_t)k.base_vertex + lo, n = (int64_t)hi - lo + 1;
    if (first < 0 || n > (1 << 20) || (uint64_t)(first + n) * k.stride > k.vsize) return 0;
    std::vector<uint8_t> v((size_t)n * k.stride);
    if (!memory_read_as_submitted(k.vbuf + (uint64_t)first * k.stride, v.data(), v.size())) return 0;
    uint32_t most = 0;
    for (int64_t i = 0; i < n; i++) {
        const uint8_t* q = v.data() + (size_t)i * k.stride;
        for (int set = 0; set < k.sets; set++)
            for (int j = 0; j < 4; j++) {
                if (!q[k.weights[set] + j]) continue;
                uint32_t joint = 0;
                if (k.formats & (dlss_actor::kByteJoints << set)) joint = q[k.joints[set] + j];
                else std::memcpy(&joint, q + k.joints[set] + 2 * j, 2);
                most = std::max(most, joint + 1);
            }
    }
    return most;
}
}  // namespace

void dlss_note_skinned_draw(const DlssSkin& k) {
    static const bool off = env_switch("RX_VK_DLSS_ACTORS") == 0;
    if (off || !dlss_wanted() || !k.depth || !k.bones || k.bones_size < 48 || !k.object || k.object_size < 0x4c) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (s.skins.size() >= 512) s.skins.clear();   // (no upscale for a while: what was kept is stale)
    struct Timed {
        State& s;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() { s.actor_ns += (std::chrono::steady_clock::now() - t0).count(); }
    } timed{s};
    State::Skin d;
    d.k = k;
    const uint64_t model = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, 0);
    // (only the bones its vertices use are taken: a palette holds up to 280, a model uses a few dozen)
    auto mb = s.model_bones.find(model);
    if (mb == s.model_bones.end()) {
        if (s.model_bones.size() > 4096) s.model_bones.clear();
        mb = s.model_bones.emplace(model, bones_used(k)).first;
    }
    d.nbones = (uint32_t)std::min<uint64_t>(mb->second, std::min<uint64_t>(k.bones_size / 48, 280));
    if (!d.nbones) return;
    int nth = 0;   // (the same model drawn twice in a frame: the n-th draw of it is the n-th the frame before)
    for (const auto& o : s.skins) nth += dlss_actor::same_model(o.key, model);
    d.key = dlss_actor::key(k.vbuf, k.ibuf, k.count, k.base_vertex, k.program, nth);
    s.skins.push_back(std::move(d));
}

bool dlss_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                  VkImageView out_view, uint32_t out_level) {
    if (!dlss_wanted()) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (!known) {   // (another network of the upscale's shape: only the scene into the picture, the rest untouched and unsaid)
        const Camera& c = s.cam;
        // (the gameplay networks' own formats: they sample the scene's B10G11R11 colour and write the B10G11R11 picture.
        // Other cooperative-matrix networks of the same shape (one RGBA8 into A2B10G10R10, RGB9E5 into B10G11R11) are not it)
        if (!in || !out || in->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
            (out->width >> out_level) <= in->width)
            return false;
        // (one in the gameplay networks' formats whose input is not this frame's scene: counted for the status line)
        if (!c.valid || texture_frame_no() - c.frame > 1 || in->width != c.width || in->height != c.height)
            return s.status_passed++, false;
        static std::atomic<uint64_t> told{0};
        if (told.exchange(key) != key) logf("vk: NVIDIA DLSS: network %016llx is the gameplay upscale (%ux%u -> %ux%u)",
                                            (unsigned long long)key, in->width, in->height, out->width >> out_level,
                                            out->height >> out_level);
    }
    free_dead(false);
    const Camera cam = s.cam;
    s.cam.valid = false;   // (each frame's own camera)
    struct SkinsDone {     // (the skinned draws, each frame's own too: actor_motion takes them, or they go here)
        State& s;
        ~SkinsDone() { s.skins.clear(); }
    } skins_done{s};

    if (s.failed) return fail("DLSS failed earlier");
    s.asked = texture_frame_no(), s.holding = true;   // (not after a failure: what it holds is let go, dlss_idle)
    if (!in || !in_view || !out || !out_view) return fail("no input or output view");
    if (!cam.valid || texture_frame_no() - cam.frame > 1) return fail("no scene camera this frame");
    if (cam.depth_gen != texture_destroy_generation()) return fail("the scene depth may be gone");
    const uint32_t rw = in->width, rh = in->height;
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    if (rw != cam.width || rh != cam.height) return fail("the input is not the scene's size");
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return fail("unexpected scene depth");
    if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || ow < rw || oh < rh || ow > rw * 3 || oh > rh * 3)
        return fail("unexpected output");
    if (!ngx_init()) {
        s.failed = true;
        return fail("DLSS not available");
    }
    if (!make_pipelines()) {
        s.failed = true;
        return fail("its shaders were not made");
    }
    const VkImageView dview = depth_view(cam.depth);
    if (!dview) return fail("no view of the scene depth");
    // (DLAA: DLSS at the render size, anti-aliasing alone; its picture then scaled to the output's by dlss_out.comp. With
    // it the 3D is drawn at full scale, dlss_full_scale)
    const bool dlaa = dlss_mode() == 2;
    const uint32_t dw = dlaa ? rw : ow, dh = dlaa ? rh : oh;
    // (from here commands are recorded, NGX's own too: whichever way this returns, the program's descriptor pushes are gone from
    // the bind point and are pushed again)
    struct Disturbed {
        ~Disturbed() { descriptors_disturbed(); }
    } disturbed;
    if (!make_feature(cmd, rw, rh, dw, dh)) return false;
    if (!make_image(cmd, s.color, VK_FORMAT_R16G16B16A16_SFLOAT, s.feature_rw, s.feature_rh) ||
        !make_image(cmd, s.depth, VK_FORMAT_R32_SFLOAT, s.feature_rw, s.feature_rh) ||
        !make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, s.feature_rw, s.feature_rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !make_image(cmd, s.output, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh))
        return fail("its images were not made");

    const uint64_t now = texture_frame_no();
    const uint64_t last = s.last_frame;
    const bool reset = !last || now - last > 2;
    struct {
        float reproject[16];
        float jitter[2];
        int32_t size[2];
        int32_t flags;
    } pc{};
    static const bool show_motion = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "motion");
    // (the motion without the jitter: from this frame's jittered clip space to the frame before's without its jitter.
    // A jitter moves clip x by jitter * w: taken off the previous matrix's first two rows)
    // (the frame before's camera as DLSS last drew it: the buffer's own slot for it, 0x180, holds zeros in this version)
    float previous[16];
    std::memcpy(previous, s.last_vp, sizeof previous);
    dlss_math::unjitter(previous, s.last_jitter);
    const bool motion = !reset && dlss_math::reprojection(cam.current, previous, pc.reproject);
    pc.jitter[0] = cam.jitter[0], pc.jitter[1] = cam.jitter[1];
    pc.size[0] = (int32_t)rw, pc.size[1] = (int32_t)rh, pc.flags = (motion ? 1 : 0) | (show_motion ? 2 : 0);

    full_barrier(cmd);   // (the scene's colour and depth, written before)
    {
        const VkDescriptorImageInfo info[5] = {{s.point, in_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, dview, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep_layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    }
    full_barrier(cmd);
    // (what moves by itself: the skinned draws drawn again over the camera's motion, with their own)
    const int actors = actor_motion(cmd, cam, previous, rw, rh, dview, motion);
    if (actors) full_barrier(cmd);
    if (show_motion && actors) {   // (RX_VK_DLSS_DEBUG=motion: the motion as it is now, the actors' in it)
        pc.flags = 4;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep);
        const VkDescriptorImageInfo info[5] = {{s.point, in_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, dview, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep_layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
        full_barrier(cmd);
    }
    NVSDK_NGX_Resource_VK color = resource(s.color, false), depth = resource(s.depth, false), mv = resource(s.motion, false),
                          output = resource(s.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color, ep.Feature.pInOutput = &output;
    ep.pInDepth = &depth, ep.pInMotionVectors = &mv;
    // (the jitter in render pixels, x right and y down)
    float jitter_px[2];
    dlss_math::jitter_to_pixels(cam.jitter, rw, rh, jitter_px);
    ep.InJitterOffsetX = jitter_px[0], ep.InJitterOffsetY = jitter_px[1];
    ep.InRenderSubrectDimensions = {rw, rh};
    ep.InReset = reset ? 1 : 0;
    ep.InMVScaleX = ep.InMVScaleY = 1.0f;
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &ep);
    note_work();   // (as at its creation: the barrier after it, and the game's next one, are recorded)
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: its evaluation failed (%#x): the game's upscale from now on", (unsigned)r);
        s.failed = true;
        return false;   // (the network writes its output over whatever the evaluation did)
    }
    full_barrier(cmd);
    {
        const int32_t opc[2] = {(int32_t)ow, (int32_t)oh};
        const VkDescriptorImageInfo info[2] = {{dlaa ? s.linear : s.point, s.output.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &info[0];
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &info[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
        vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    }
    s.last_frame = now;
    s.last_jitter[0] = cam.jitter[0], s.last_jitter[1] = cam.jitter[1];
    // (for the status line, dlss_status: the jitter DLSS was given, in render pixels)
    s.status_jitter[0] = ep.InJitterOffsetX, s.status_jitter[1] = ep.InJitterOffsetY, s.status_motion = motion;
    s.status_drawn++;

    std::memcpy(s.last_vp, cam.current, sizeof s.last_vp);
    s.jitter_w = rw, s.jitter_h = rh, s.jitter_phases = (uint32_t)dlss_jitter_phases(rw, rh, dw, dh);
    if (s.reason) logf("vk: NVIDIA DLSS draws the upscale again (frame %llu)", (unsigned long long)now);
    s.reason = nullptr;
    // (RX_VK_DLSS_DEBUG=alternate, a test's reference: every other frame the game's own upscale over DLSS's, which still
    // draws and keeps its history. A frame of DLSS then has the game's picture of the frames either side to be measured against)
    static const bool alternate = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "alternate");
    if (alternate && (now & 1)) return false;
    if (!s.told_first) {
        s.told_first = true;
        logf("vk: NVIDIA DLSS draws OoT's upscale: %ux%u -> %ux%u%s", rw, rh, ow, oh, show_motion ? " (its motion shown)" : "");
    }
    return true;
}

#else  // (no NGX SDK in this build: DLSS is not there)

bool dlss_built() { return false; }
void dlss_probe() {}
void dlss_instance_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_device_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_note_scene_draw(const Texture*, uint32_t, uint32_t, Texture*, uint64_t, uint64_t) {}
bool dlss_upscale(VkCommandBuffer, uint64_t, bool, Texture*, VkImageView, Texture*, VkImageView, uint32_t) { return false; }
void dlss_idle() {}
int dlss_status(char*, size_t) { return 0; }
uint32_t dlss_jitter_size(int) { return 0; }
void dlss_note_skinned_draw(const DlssSkin&) {}

#endif

} // namespace mkwvk
