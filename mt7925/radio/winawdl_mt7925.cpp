// SPDX-License-Identifier: GPL-2.0-only
#include <iostream>
#include <memory>
#include "mt7925_device.h"
#include "mt7925_bridge.h"
#include "mt7925_shutdown.h"

namespace {
std::atomic<bool> stop_requested{false};
BOOL WINAPI interrupted(DWORD event) {
  if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) {stop_requested=true;return TRUE;}
  return FALSE;
}
int number(const std::wstring& s) {
  size_t end=0;int n=std::stoi(s,&end);
  if(end!=s.size())throw std::runtime_error("invalid numeric argument");return n;
}
}
int wmain(int argc,wchar_t** argv) {
  if(argc==1 || (argc==2 && std::wstring(argv[1])==L"--help")) {
    std::cout<<"MT7925 native radio-link v3 backend (experimental)\n"
      "--run --instance PCI_INSTANCE_ID --firmware DIR --country 00 --channel 6 --power-half-dbm 12 --fixed-channel\n"
      "MT7925 E138 revision 00, 20 MHz. Optional: --seconds 1..600.\n"
      "--stop-file PATH requests a graceful STOP when the supervisor creates the file.\n"
      "--capture PATH records filtered frame headers (32 MiB maximum); TX is local acceptance only.\n"
      "--run starts firmware/monitor and allows client RF transmission. Never installs or elevates.\n"
      "Own MAC is programmed on SET_ADDRESS; actual peer ACK/AirDrop are unverified.\n";
    return 0;
  }
  try {
    mt7925::Config c{{'0','0'},0,0,0,20,0,0};
    std::filesystem::path folder,stop_file,capture_file;
    std::wstring instance;
    bool run=false,country=false,channel=false,power=false,env=false;
    int seconds=0;bool fixed_channel=false;
    for(int i=1;i<argc;++i) {
      const std::wstring key=argv[i];
      if(key==L"--run" && !run){run=true;continue;}
      if(key==L"--fixed-channel" && !fixed_channel){fixed_channel=true;continue;}
      if(i+1>=argc)throw std::runtime_error("missing argument value");
      const std::wstring value=argv[++i];
      if(key==L"--instance" && instance.empty())instance=value;
      else if(key==L"--firmware" && folder.empty())folder=value;
      else if(key==L"--stop-file" && stop_file.empty())stop_file=value;
      else if(key==L"--capture" && capture_file.empty())capture_file=value;
      else if(key==L"--country" && !country && value.size()==2 && value[0]<=127 && value[1]<=127) {
        c.country={uint8_t(value[0]),uint8_t(value[1])};country=true;
      } else if(key==L"--channel" && !channel) {c.primary=c.center=uint32_t(number(value));channel=true;}
      else if(key==L"--power-half-dbm" && !power) {c.power=number(value);power=true;}
      else if(key==L"--environment" && !env) {c.environment=uint32_t(number(value));env=true;}
      else if(key==L"--seconds" && !seconds) {
        seconds=number(value);
        if(seconds<1 || seconds>600)throw std::runtime_error("trial seconds must be 1..600");
      }
      else throw std::runtime_error("unknown or duplicate option");
    }
    if(!run || instance.empty() || folder.empty() || !country || !channel || !power || !fixed_channel)
      throw std::runtime_error("required E138 radio configuration missing (see --help)");
    if(c.country!=std::array<uint8_t,2>{'0','0'} || c.environment || c.primary!=6 || c.power>12 || c.power < -127)
      throw std::runtime_error("E138 requires the world domain, channel 6/20 MHz and at most 6 dBm");
    if(!stop_file.empty() && (!stop_file.is_absolute() || mt7925::stop_file_present(stop_file)))
      throw std::runtime_error("stop file requires an absent absolute path");
    std::unique_ptr<FrameCapture> capture;
    if(!capture_file.empty()) {
      if(!capture_file.is_absolute() || !seconds || std::filesystem::exists(capture_file))
        throw std::runtime_error("capture requires an absent absolute path and a bounded runtime");
      capture=std::make_unique<FrameCapture>(capture_file.string());
      if(!capture->is_open())throw std::runtime_error("cannot create capture file");
    }
    auto request=mt7925::start_request(mt7925::firmware_bundle(folder),c);
    const auto deadline=seconds?GetTickCount64()+uint64_t(seconds)*1000:0;
    auto stopping=[deadline,stop_file]{
      if(deadline && GetTickCount64()>=deadline)stop_requested=true;
      if(mt7925::stop_file_present(stop_file))stop_requested=true;
      return stop_requested.load();
    };
    PipeServer pipe(RADIO_LINK_PIPE_NAME,stopping);
    if(!pipe.valid())throw std::runtime_error("radio pipe unavailable (another backend may be running)");
    // NekoDrop starts children without a console and requests STOP through
    // the session's stop file. An interactive launch also supports Ctrl+C.
    DWORD console_process;
    if(GetConsoleProcessList(&console_process,1) && !SetConsoleCtrlHandler(interrupted,TRUE))
      throw mt7925::win_error("register stop handler");
    mt7925::Device driver(instance);
    mt7925::Bridge session(driver,c.primary,stopping,std::chrono::milliseconds(2000),fixed_channel,capture.get());
    std::exception_ptr failure;
    try {
      session.start(request);
      std::cout<<"Local radio initialized; waiting for AWDL on channel 6 (20 MHz)."<<std::endl;
      mt7925::serve(pipe,session,uint8_t(c.primary),stop_requested);
    } catch(const mt7925::StopRequested&) {
      std::cout<<"Radio runtime limit reached or stop requested."<<std::endl;
    } catch(...) {failure=std::current_exception();}
    stop_requested=true;
    // serve has joined the polling worker; this report does not call the driver.
    try {std::cerr<<"Native RX diagnostics JSON: ";session.rx_diagnostics().write_json(std::cerr);std::cerr<<'\n';}
    catch(...) {std::cerr<<"Native RX diagnostics output failed.\n";}
    // Also attempted after a failed START. Kernel handle cleanup is a fallback,
    // not a substitute for reporting a failed explicit stop.
    mt7925::finish_session(session,driver,failure,bool(capture),std::cout,std::cerr);
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
