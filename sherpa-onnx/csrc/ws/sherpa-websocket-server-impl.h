// sherpa-onnx/csrc/online-websocket-server-impl.h
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#ifndef SHERPA_ONNX_CSRC_WEBSOCKET_SERVER_IMPL_H_
#define SHERPA_ONNX_CSRC_WEBSOCKET_SERVER_IMPL_H_

#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>  // NOLINT
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "sherpa-onnx/csrc/offline-tts.h"
#include "sherpa-onnx/csrc/online-recognizer.h"
#include "sherpa-onnx/csrc/offline-recognizer.h"
#include "sherpa-onnx/csrc/voice-activity-detector.h"
#include "sherpa-onnx/csrc/parse-options.h"
#include "sherpa-onnx/csrc/resample.h"
#include "hv/WebSocketChannel.h"  // NOLINT
#include "hv/WebSocketServer.h"  // NOLINT
#include "hv/EventLoopThreadPool.h"
extern "C" {
#include "shine_mp3.h"
}

using connection_hdl = WebSocketChannelPtr;

namespace sherpa_onnx {
enum WavFmt { eFloat, eShort, eByte };
using AutoLock = std::lock_guard<std::recursive_mutex>;
struct Connection : public std::enable_shared_from_this<Connection> {
  hv::EventLoop *worker_ = nullptr;
  // asr handle
  std::shared_ptr<OnlineStream> son;
  std::unique_ptr<VoiceActivityDetector> vad_;
  std::recursive_mutex mtx;
  // set it to true when InputFinished() is called
  bool eof = false;
  // tts text line for output
  std::list<std::string> tts_lines_;
  // cur output line
  std::string tts_line_;
  
  shine_t mp3_enc_ = nullptr;
  // binary send buffer
  std::deque<std::string> tts_wavs_;

  hv::Json llm_ctx_;
  HttpRequestPtr llm_req_;
  std::wstring llm_line_;
  unsigned int llm_s_ = 0, llm_f_ = 0;

  int in_sample_rate = 16000;  // in sample rate for asr
  int out_sample_rate = 16000;  // out sample rate for tts
  int out_frame_size = 960; // 60ms
  hv::TimerID tts_id_ = 0; // tts frame send id
  WavFmt fmt = eShort; // audio format for send and recv
  std::vector<float> tts_cache_;
  std::unique_ptr<LinearResample> resample_;
  // 打断计数器
  volatile int req_index_ = 0;
  bool sameReq(int idx) {
    AutoLock lock(mtx);
    return req_index_ == idx;
  }
  // 增加索引，并触发打断
  int doBreak();
  void addTtsWav(const float *data, int size, int samplerate);
  void addTtsFrame(const float *data, int size);
  void addTtsFrame(const std::string &frame);
  bool popTtsFrame(std::string &frame);
  Connection() = default;
  ~Connection();
  bool openCodec(int samplerate, int channel, int bitrate, int num = 1);
  void closeCodec();
  void stop();
};

struct WebsocketServerConfig {
  OnlineRecognizerConfig online_config;
  OfflineRecognizerConfig offline_config;
  OfflineTtsConfig tts_config;
  VadModelConfig vad_config;
  std::string llm_url, llm_model, llm_key;
  std::string llm_type;
  int mp3_bitrate = 64; // 64kbps
  int mp3_frame_count = 1;
  int tts_frame_count = 30;
  int tts_frame_size = 960;
  void Register(sherpa_onnx::ParseOptions *po);
  void Validate() const;
};

class SherpaWebsocketServer : public WebSocketService {
 public:
  explicit SherpaWebsocketServer(hv::EventLoopThreadPool* io_work,  // NOLINT
                                 const WebsocketServerConfig &config);

  void Run();

  const WebsocketServerConfig &GetConfig() const { return config_; }
  hv::EventLoopThreadPool* GetWorkContext() { return io_work_; }

  OnlineRecognizer *asrOnline() { return asr_online_.get(); }
  OfflineRecognizer *asrOffline() { return asr_offline_.get(); }
  OfflineTts *tts() { return tts_.get(); }
 private:
  // When a websocket client is connected, it will invoke this method
  // (Not for HTTP)
  void OnOpen(connection_hdl hdl, const HttpRequestPtr &req);
  void OnMessage(connection_hdl hdl, const std::string& msg);
  // do in worker loop
  void doAsr(connection_hdl hdl, const std::string &msg);
  void doTts(connection_hdl hdl, int idx);
  void doTts(connection_hdl hdl);
  void doLlm(connection_hdl hdl, const std::string &msg);
  // 增加tts文本输出
  void addTts(connection_hdl hdl, const std::string &msg, bool done = true);
  void onAsrLine(connection_hdl hdl, const std::string& line);
 private:
  // 发送tts语音和文本帧
  void sendTtsFrame(connection_hdl hdl);

  std::unique_ptr<OfflineTts> tts_;
  std::unique_ptr<OnlineRecognizer> asr_online_;
  std::unique_ptr<OfflineRecognizer> asr_offline_;
  WebsocketServerConfig config_;
  hv::EventLoopThreadPool *io_work_;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_WEBSOCKET_SERVER_IMPL_H_
