"""Compile actual swapchain lifecycle/selection statements with CPU Vulkan stubs.
Usage: python -I -B tests/test_present_semaphores.py [--cxx clang++]
The old source must fail the four-images/three-frame-slots sequence. No GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
p.add_argument('--cxx', default='clang++')
a = p.parse_args()
source = a.source.resolve()
main = (source/'gpu/vk/vk_main.cpp').read_text()
replay = (source/'gpu/vk/vk_replay.cpp').read_text()
present = replay[replay.index('void present_image('):replay.index('void present_image(')+12000]
signal = re.search(r'    signal\.semaphore = [^;]+;', present)[0]
wait = re.search(r'    pi\.pWaitSemaphores = [^;]+;', present)[0]
acquire_wait = re.search(r'    waits\[0\]\.semaphore = [^;]+;', present)[0]
assert 'acquire_timeout, f.acquired, VK_NULL_HANDLE, &index)' in present
# (from the image query after the swapchain is made: since PR 492 its creation is a result checked and logged)
start = main.index('    if (vkGetSwapchainImagesKHR(dev.device, sc, &n, nullptr)')
end = main.index('    logf("vk: swapchain', start)
tail = main[start:end]
fixture = r'''
#include <vector>
#include <set>
#include <cstdint>
#include <cstdio>
#include <utility>
using VkSemaphore=uint64_t; using VkImage=uint64_t;
constexpr uint64_t VK_NULL_HANDLE=0;
constexpr int VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO=1, VK_SUCCESS=0;
struct VkSemaphoreCreateInfo { int type; };
struct Extent { uint32_t width,height; };
struct Device { int device=1;uint64_t swapchain=0;int swap_format=0;Extent swap_extent{};
    std::vector<VkImage> swap_images;std::vector<VkSemaphore> swap_rendered; } dev;
unsigned image_count=4, made=0, fail_at=0;uint64_t serial=1000;
int query_result=0;uint64_t destroyed_swapchain=0;
bool idle=false, bad_destroy=false; std::set<uint64_t> live,used;
int vkGetSwapchainImagesKHR(int,uint64_t,uint32_t* n,uint64_t* out) {
    *n=image_count;if(out)for(unsigned i=0;i<*n;++i)out[i]=i;return query_result;
}
int vkCreateSemaphore(int,const VkSemaphoreCreateInfo*,void*,uint64_t* out) {
    if(++made==fail_at)return -1;*out=++serial;live.insert(*out);return 0;
}
void vkDestroySemaphore(int,uint64_t s,void*) {
    if((used.count(s) && !idle) || !live.erase(s))bad_destroy=true;
}
void vkDestroySwapchainKHR(int,uint64_t sc,void*) {destroyed_swapchain=sc;}
void queue_flush_locked() {}
void vkDeviceWaitIdle(int) {idle=true;}
bool recreate(uint64_t sc) {
    uint32_t n=0,width=640,height=480; struct {int format=1;} fmt;
''' + tail + r'''
    return true;
}
struct Frame {uint64_t rendered,acquired;};
uint64_t choose(unsigned index,unsigned frame_slot) {
    Frame f{100+frame_slot,200+frame_slot};struct{uint64_t semaphore;}signal{};
    struct{uint64_t semaphore;}waits[1]{};
    struct{const uint64_t* pWaitSemaphores;}pi{};
''' + signal + '\n' + wait + '\n' + acquire_wait + r'''
    if(signal.semaphore!=*pi.pWaitSemaphores)return 0;
    if(waits[0].semaphore!=200+frame_slot)return 0;
    used.insert(signal.semaphore);
    return signal.semaphore;
}
int main() {
    if(!recreate(1))return 1;
    uint64_t pending[16]{};unsigned owner[16]{},count=0;
    // Only reacquiring image i releases its previous presentation wait.
    unsigned images[]={0,1,2,3,0,1,2,3,3,1,0,2};
    for(unsigned step=0;step<12;++step) {
        unsigned image=images[step];
        for(unsigned j=0;j<count;++j)if(owner[j]==image)pending[j]=0;
        auto sem=choose(image,step%3);if(!sem)return 2;
        for(unsigned j=0;j<count;++j)if(pending[j]==sem) {
            std::printf("FAIL: slot %u reused semaphore for image %u while image %u still owns it\n",step%3,image,owner[j]);return 3;
        }
        pending[count]=sem;owner[count++]=image;
    }
    if(dev.swap_rendered.size()!=4 || live.size()!=4)return 4;
    auto old=dev.swap_rendered;image_count=2;idle=false;
    if(!recreate(2) || bad_destroy || dev.swap_rendered.size()!=2 || live.size()!=2)return 5;
    for(auto s:old)if(live.count(s))return 6;
    for(unsigned slot=0;slot<3;++slot)for(unsigned image=0;image<2;++image)
        if(choose(image,slot)!=dev.swap_rendered[image])return 9;
    image_count=5;idle=false;
    if(!recreate(3) || bad_destroy || dev.swap_rendered.size()!=5 || live.size()!=5)return 10;
    for(unsigned image=0;image<5;++image)if(choose(image,image%3)!=dev.swap_rendered[image])return 11;
    // A successful CreateSwapchain retires the old chain, even if later setup fails.
    // Only test resource cleanup/ownership here, never resumption on that retired chain.
    old=dev.swap_rendered;auto previous=dev.swapchain;image_count=4;made=0;fail_at=3;
    idle=false;
    if(recreate(4) || destroyed_swapchain!=4 || dev.swapchain!=previous || dev.swap_rendered!=old || live.size()!=5)return 7;
    fail_at=0;made=0;query_result=-1;destroyed_swapchain=0;
    if(recreate(5) || destroyed_swapchain!=5 || made || dev.swap_rendered!=old || live.size()!=5)return 12;
    query_result=0;image_count=0;destroyed_swapchain=0;
    if(recreate(6) || destroyed_swapchain!=6 || made || dev.swap_rendered!=old || live.size()!=5)return 13;
    if(bad_destroy)return 8;
    std::puts("PASS: acquired-image reuse, frame acquire, recreation, allocation failure and image-query error/zero cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='present-semaphores-') as directory:
    folder = Path(directory)
    cpp, exe = folder/'fixture.cpp', folder/'fixture.exe'
    cpp.write_text(fixture)
    subprocess.run([a.cxx, '-std=c++20', '-O1', str(cpp), '-o', str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
