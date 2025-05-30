// sherpa-onnx/csrc/offline-websocket-server.cc
//
// Copyright (c)  2022-2023  Xiaomi Corporation

#include "hv/WebSocketServer.h"
#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/offline-websocket-server-impl.h"
#include "sherpa-onnx/csrc/parse-options.h"

static constexpr const char *kUsageMessage = R"(
Automatic speech recognition with sherpa-onnx using websocket.

Usage:

./bin/sherpa-onnx-offline-websocket-server --help

(1) For transducer models

./bin/sherpa-onnx-offline-websocket-server \
  --port=6006 \
  --num-work-threads=5 \
  --tokens=/path/to/tokens.txt \
  --encoder=/path/to/encoder.onnx \
  --decoder=/path/to/decoder.onnx \
  --joiner=/path/to/joiner.onnx \
  --log-file=./log.txt \
  --max-batch-size=5

(2) For Paraformer

./bin/sherpa-onnx-offline-websocket-server \
  --port=6006 \
  --num-work-threads=5 \
  --tokens=/path/to/tokens.txt \
  --paraformer=/path/to/model.onnx \
  --log-file=./log.txt \
  --max-batch-size=5

Please refer to
https://k2-fsa.github.io/sherpa/onnx/pretrained_models/index.html
for a list of pre-trained models to download.
)";

int32_t main(int32_t argc, char *argv[]) {
  sherpa_onnx::ParseOptions po(kUsageMessage);

  sherpa_onnx::OfflineWebsocketServerConfig config;

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
  po.DisableOption("sample-rate");

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

  sherpa_onnx::OfflineWebsocketServer service(&io_work, config);
  server.registerWebSocketService(&service);

  hv::HttpService http;
  //http.Static("/", "./static");
  http.POST("/", [&](const HttpRequestPtr &req, const HttpResponseWriterPtr &writer) {
    int samplerate = req->Get("samplerate", 16000);
    std::string payload = req->GetFormData("file");
    auto decoder = service.handle();
    io_work.loop()->runInLoop([decoder, samplerate, payload, writer]() {
        auto stream = decoder->CreateStream();
        stream->AcceptWaveform(samplerate, (float *)payload.data(),
                                payload.length() / sizeof(float));
        decoder->DecodeStream(stream.get());
        auto res = stream->GetResult();
        writer->Begin();
        writer->EndHeaders("Content-Type", "application/json");
        writer->WriteBody(res.AsJsonString());
        writer->close();
    });
  });
  server.registerHttpService(&http);
  server.start();

  SHERPA_ONNX_LOGE("Started!");
  SHERPA_ONNX_LOGE("Listening on: %d", port);
  SHERPA_ONNX_LOGE("Number of work threads: %d", num_work_threads);

  printf("presee q to exit\n");
  while (getchar() != 'q');

  return 0;
}
