"""Execute the actual prepare-only completion branch with game-free call stubs."""
import argparse, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--cxx',default='clang++');a=p.parse_args()
s=(a.source/'gpu/vk/vk_main.cpp').read_text();start=s.index('if (prepare_only()) {');at=s.index('{',start)+1;end=at;depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
body=s[at:end-1]
cpp=r"""
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using std::size_t;
std::vector<int> calls;
bool prepare_run(void(*)(size_t,size_t),bool background=false){if(background)std::exit(3);calls.push_back(1);return true;}
void predict_run(void(*)(size_t,size_t)){calls.push_back(5);}
void precompile_run(void(*)(size_t,size_t)){calls.push_back(2);}
void pipeline_cache_save(bool force){if(!force)std::exit(2);calls.push_back(3);}
std::string skipped_path(){return "sentinel";}
int fake_remove(const char*){calls.push_back(4);return 0;}
#define remove fake_remove
bool run(){
"""+body+r"""
}
int main(){if(!run()||calls!=std::vector<int>{1,5,2,3,4}){std::fputs("FAIL: preparation must synchronously force cache save before clearing skipped marker\n",stderr);return 1;}std::puts("PASS: actual preparation branch forces save after precompile before marker cleanup");}
"""
with tempfile.TemporaryDirectory(prefix='prepare-cache-') as tmp:
    src=Path(tmp)/'fixture.cpp';exe=Path(tmp)/'fixture.exe';src.write_text(cpp)
    subprocess.run([a.cxx,'-std=c++20',str(src),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
