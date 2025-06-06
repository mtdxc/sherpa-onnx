// sherpa-onnx/csrc/online-websocket-server.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "hv/WebSocketServer.h"
#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-websocket-server-impl.h"
#include "sherpa-onnx/csrc/parse-options.h"

static constexpr const char *kUsageMessage = R"(
Automatic speech recognition with sherpa-onnx using websocket.

Usage:

./bin/sherpa-onnx-websocket-server --help

./bin/sherpa-onnx-websocket-server \
  --port=6006 \
  --num-work-threads=5 \
  --tokens=/path/to/tokens.txt \
  --encoder=/path/to/encoder.onnx \
  --decoder=/path/to/decoder.onnx \
  --joiner=/path/to/joiner.onnx \
  --log-file=./log.txt \
  --max-batch-size=5 \
  --loop-interval-ms=10

Please refer to
https://k2-fsa.github.io/sherpa/onnx/pretrained_models/index.html
for a list of pre-trained models to download.
)";

int32_t main(int32_t argc, char *argv[]) {
#ifdef _WIN32
  //SetConsoleCP(65001);
  SetConsoleOutputCP(65001);
#endif  // _WIN32

  sherpa_onnx::ParseOptions po(kUsageMessage);
  hlog_set_handler(stdout_logger);
  hlog_set_level(LOG_LEVEL_INFO);
  sherpa_onnx::WebsocketServerConfig config;

  // the server will listen on this port
  int32_t port = 6006;

  // size of the thread pool for handling network connections
  int32_t num_io_threads = 1;

  // size of the thread pool for neural network computation and decoding
  int32_t num_work_threads = 3;

  po.Register("num-io-threads", &num_io_threads,
              "Thread pool size for network connections.");

  po.Register("num-work-threads", &num_work_threads,
              "Thread pool size for for neural network "
              "computation and decoding.");

  po.Register("port", &port, "The port on which the server will listen.");

  std::string s_certfile, s_keyfile;
  po.Register("cert", &s_certfile, "ssl cert file.");
  po.Register("key", &s_keyfile, "ssl key file.");

  config.Register(&po);

  if (argc == 1) {
    po.PrintUsage();
    exit(EXIT_FAILURE);
  }

  po.Read(argc, argv);

  if (po.NumArgs() != 0) {
    SHERPA_ONNX_LOGE("Unrecognized positional arguments!");
    po.PrintUsage();
    exit(EXIT_FAILURE);
  }

  config.Validate();

  hv::EventLoopThreadPool io_work;
  io_work.setThreadNum(num_work_threads);
  io_work.start();
  sherpa_onnx::SherpaWebsocketServer service(&io_work, config);
  hv::WebSocketServer server;
  server.setThreadNum(num_io_threads);
  server.port = port;

  if (s_certfile.length()) {
    server.https_port = port + 1;
    hssl_ctx_opt_t param;
    memset(&param, 0, sizeof(param));
    param.crt_file = s_certfile.c_str();
    param.key_file = s_keyfile.c_str();
    param.endpoint = HSSL_SERVER;
    if (server.newSslCtx(&param) != 0) {
      fprintf(stderr, "new SSL_CTX failed!\n");
      return -20;
    }
  }
  hv::HttpService http;
  http.Static("/", "./assets");
  http.POST("/", [&](const HttpRequestPtr &req, const HttpResponseWriterPtr &writer) {
    int samplerate = req->Get("samplerate", 16000);
    std::string payload = req->GetFormData("file");
    io_work.loop()->runInLoop([&service, samplerate, payload, writer]() {
      std::string json;
      if (auto decoder = service.asrOnline()) {
        auto stream = decoder->CreateStream();
        stream->AcceptWaveform(samplerate, (float *)payload.data(),
                               payload.length() / sizeof(float));
        stream->InputFinished();
        decoder->DecodeStream(stream.get());
        auto res = decoder->GetResult(stream.get());
        json = res.AsJsonString();
      } else if(auto dec = service.asrOffline()) {
        auto stream = dec->CreateStream();
        stream->AcceptWaveform(samplerate, (float *)payload.data(),
                               payload.length() / sizeof(float));
        dec->DecodeStream(stream.get());
        json = stream->GetResult().AsJsonString();
      }
      writer->Begin();
      writer->EndHeaders("Content-Type", "application/json");
      writer->WriteBody(json);
      writer->close();
    });
  });
  server.registerHttpService(&http);

  server.registerWebSocketService(&service);
  //server.onWorkerStart = [&service]() { service.Run(); };

  SHERPA_ONNX_LOGE("Started!");
  SHERPA_ONNX_LOGE("Listening on: %d", port);
  SHERPA_ONNX_LOGE("Number of work threads: %d", num_work_threads);

  server.run();
  io_work.stop(true);
  return 0;
}
