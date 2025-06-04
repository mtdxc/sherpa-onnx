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
#include "sherpa-onnx/csrc/online-stream.h"
#include "sherpa-onnx/csrc/parse-options.h"
#include "sherpa-onnx/csrc/tee-stream.h"
#include "hv/WebSocketChannel.h"  // NOLINT
#include "hv/WebSocketServer.h"  // NOLINT
#include "hv/EventLoopThreadPool.h"
using connection_hdl = WebSocketChannelPtr;

namespace sherpa_onnx {
enum WavFmt { eFloat, eShort, eByte };
struct Connection : public std::enable_shared_from_this<Connection> {
  // handle to the connection. We can use it to send messages to the client
  std::shared_ptr<OnlineStream> s;
  std::shared_ptr<OfflineStream> os;

  // set it to true when InputFinished() is called
  bool eof = false;

  hv::EventLoop *worker_ = nullptr;
  std::list<std::string> tts_lines_;
  std::string tts_line_;
  int tts_index_ = 0;
  std::unique_ptr<VoiceActivityDetector> vad_;
  // TTS audio samples received from the client.
  // ÿ֡һ��
  std::deque<std::string> tts_wavs_;
  bool popTtsFrame(std::string& frame);
  void onAsrLine(std::string line);
  int samplerate = 16000;  // default sample rate
  WavFmt fmt = eShort;
  Connection() = default;
};

struct WebsocketServerConfig {
  OnlineRecognizerConfig online_config;
  OfflineRecognizerConfig offline_config;
  OfflineTtsConfig tts_config;
  VadModelConfig vad_config;
  std::string log_file = "./log.txt";

  void Register(sherpa_onnx::ParseOptions *po);
  void Validate() const;
};

class SherpaWebsocketServer : public WebSocketService {
 public:
  explicit SherpaWebsocketServer(hv::EventLoopThreadPool* io_work,  // NOLINT
                                 const WebsocketServerConfig &config);

  void Run(uint16_t port);

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

  void doAsr(connection_hdl hdl, const std::string &msg);
  void doTts(connection_hdl hdl, const std::string &msg);
 private:
  std::unique_ptr<OfflineTts> tts_;
  std::unique_ptr<OnlineRecognizer> asr_online_;
  std::unique_ptr<OfflineRecognizer> asr_offline_;
  WebsocketServerConfig config_;
  hv::EventLoopThreadPool *io_work_;
};

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_WEBSOCKET_SERVER_IMPL_H_
