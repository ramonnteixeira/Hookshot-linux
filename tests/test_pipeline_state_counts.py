"""Extract the real state-list reader; reject bad counts but keep valid records.

Synthetic records only. No Vulkan, game, or ETW API. Run with --source ROOT.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def block(text, marker):
    start=text.index(marker);brace=text.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=ROOT)
    parser.add_argument('--cxx',default='clang++')
    args=parser.parse_args()
    text=(args.source/'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    actual='\n'.join((block(text,'constexpr char kStateMagic[8]')+';',block(text,'struct StateRec {')+';',
                       block(text,'std::vector<StateRec> state_list_read(')))
    fixture=r'''
#define _CRT_SECURE_NO_WARNINGS 1
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
'''+actual+r'''
static void write(const std::filesystem::path& path,const std::vector<StateRec>& records,size_t tail=0) {
 std::ofstream out(path,std::ios::binary);
 const uint32_t size=sizeof(StateRec);
 out.write(kStateMagic,8);out.write(reinterpret_cast<const char*>(&size),4);
 for(const auto& r:records)out.write(reinterpret_cast<const char*>(&r),sizeof r);
 if(tail){StateRec r{};out.write(reinterpret_cast<const char*>(&r),tail);}
 if(!out)throw std::runtime_error("synthetic file write failed");
}
int main(int argc,char** argv) {
 if(argc!=2)return 2;
 static_assert(sizeof(StateRec)==528);
 StateRec maximum{},zero{},bad_answers{},bad_questions{},huge{};
 maximum.code[0]=11;maximum.nanswers=24;maximum.nquestions=32;
 for(uint32_t i=0;i<24;i++)maximum.answers[i]=100+i;
 for(uint32_t i=0;i<32;i++)maximum.questions[i]=uint8_t(i);
 zero.code[0]=22;
 bad_answers=maximum;bad_answers.nanswers=25;
 bad_questions=maximum;bad_questions.nquestions=33;
 huge=maximum;huge.nanswers=huge.nquestions=std::numeric_limits<uint32_t>::max();
 int failures=0;
 auto check=[&](const std::filesystem::path& path,const std::vector<StateRec>& expected,const char* label){
  const auto got=state_list_read(path.string());
  bool equal=got.size()==expected.size();
  if(equal)for(size_t i=0;i<got.size();++i)equal&=std::memcmp(&got[i],&expected[i],sizeof(StateRec))==0;
  if(!equal){std::cerr<<label<<": reader returned "<<got.size()<<" records, expected "<<expected.size()<<'\n';++failures;}
 };
 // Both preparation inputs and state_record's read-before-append share this exact reader.
 for(const char* name:{"hookshot_pipeline_states.bin","hookshot_pipeline_states_shipped.bin"}){
  const auto path=std::filesystem::path(argv[1])/name;
  write(path,{maximum});check(path,{maximum},"valid maximum counts");
  write(path,{bad_answers});check(path,{},"answer count above array");
  write(path,{bad_questions});check(path,{},"question count above array");
  write(path,{huge});check(path,{},"maximum integer counts");
  write(path,{maximum,bad_answers,zero,bad_questions,maximum,huge,zero});
  check(path,{maximum,zero,maximum,zero},"retain valid records around rejected records");
  write(path,{maximum},sizeof(StateRec)-1);check(path,{maximum},"ignore truncated trailing record");
  write(path,{},sizeof(StateRec)-1);check(path,{},"truncated record only");
 }
 if(!failures)std::cout<<"PASS: actual reader bounds counts, retains valid records, ignores truncated tails; player and shipped names\n";
 return failures?1:0;
}
'''
    with tempfile.TemporaryDirectory(prefix='state-reader-counts-') as tmp:
        folder=Path(tmp);source=folder/'fixture.cpp';exe=folder/'fixture.exe'
        source.write_text(fixture,encoding='utf-8')
        subprocess.run([args.cxx,'-std=c++20','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe),str(folder)],check=True)

if __name__=='__main__':main()
