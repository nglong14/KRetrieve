// Command-line entry point and role selection.
#include "coordinator.hpp"
#include "index.hpp"
#include "loadgen.hpp"
#include "net.hpp"
#include "prepare.hpp"
#include "protocol.hpp"
#include "server.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s <role> [args]\n"
               "roles:\n"
               "  prepare --input <corpus.tsv> --output <new-directory> --shards <N>\n"
               "  shard --stats <global_stats.bin> --partition <shard-N.tsv> --port <P>\n"
               "        [--host 127.0.0.1] [--max-connections 64] [--workers <N>]\n"
               "        [--max-queue 16] [--latency-ms <A>[-<B>]] [--latency-prob 1]\n"
               "  query --port <P> --query <text> [--host 127.0.0.1] [--k 10] [--budget-us 0]\n"
               "  coordinator --shards <host:port,...> --port <P>\n"
               "        [--host 127.0.0.1] [--max-connections 64] [--workers <N>]\n"
               "        [--max-queue 16]\n"
               "  loadgen --port <P> --qps <N> --duration <seconds> --queries <file>\n"
               "        --out <results.csv> [--host 127.0.0.1] [--k 10] [--budget-us 0]\n"
               "        [--connections 32]\n",
               argv0);
  return 2;
}

using Options = std::map<std::string, std::string, std::less<>>;

// Parse "--name value" pairs; reject unknown, duplicate, valueless, and empty options.
Options parse_options(int argc, char** argv, std::initializer_list<std::string_view> known) {
  Options options;
  for (int i = 2; i < argc; i += 2) {
    const std::string option = argv[i];
    if (i + 1 >= argc) throw std::invalid_argument("missing value for " + option);
    const std::string_view value = argv[i + 1];
    if (value.empty()) throw std::invalid_argument("empty value for " + option);
    bool is_known = false;
    for (const auto name : known) is_known = is_known || name == option;
    if (!is_known || !options.emplace(option, value).second) {
      throw std::invalid_argument("unknown or duplicate option: " + option);
    }
  }
  return options;
}

std::string required(const Options& options, std::string_view name) {
  const auto found = options.find(name);
  if (found == options.end()) throw std::invalid_argument(std::string(name) + " is required");
  return found->second;
}

std::string optional(const Options& options, std::string_view name, std::string fallback) {
  const auto found = options.find(name);
  return found == options.end() ? fallback : found->second;
}

template <typename T>
T parse_number(const std::string& value, std::string_view name, T min) {
  T out{};
  const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || out < min) {
    throw std::invalid_argument(std::string(name) +
                                " must be an integer >= " + std::to_string(min) + " within range");
  }
  return out;
}

size_t max_connections(const Options& options) {
  return parse_number<size_t>(optional(options, "--max-connections", "64"), "--max-connections", 1);
}

// 0 lets the server pick its default.
size_t workers(const Options& options) {
  return options.contains("--workers")
             ? parse_number<size_t>(required(options, "--workers"), "--workers", 1)
             : 0;
}

size_t max_queue(const Options& options) {
  return parse_number<size_t>(
      optional(options, "--max-queue", std::to_string(kretrieve::FrameServer::kDefaultMaxQueue)),
      "--max-queue", 0);
}

// Injected shard latency for experiments: with probability `prob`, sleep min_ms..max_ms.
struct InjectedLatency {
  uint32_t min_ms = 0;
  uint32_t max_ms = 0;
  double prob = 1;
};

// Parses --latency-ms <A> or <A>-<B> and --latency-prob <P>; nullopt when not requested.
std::optional<InjectedLatency> injected_latency(const Options& options) {
  if (!options.contains("--latency-ms")) {
    if (options.contains("--latency-prob")) {
      throw std::invalid_argument("--latency-prob needs --latency-ms");
    }
    return std::nullopt;
  }
  const std::string range = required(options, "--latency-ms");
  const auto dash = range.find('-');
  InjectedLatency latency;
  latency.min_ms = parse_number<uint32_t>(range.substr(0, dash), "--latency-ms", 0);
  latency.max_ms = dash == std::string::npos
                       ? latency.min_ms
                       : parse_number<uint32_t>(range.substr(dash + 1), "--latency-ms", 0);
  if (latency.max_ms < latency.min_ms) throw std::invalid_argument("--latency-ms needs A <= B");
  const std::string prob = optional(options, "--latency-prob", "1");
  const auto parsed = std::from_chars(prob.data(), prob.data() + prob.size(), latency.prob);
  if (parsed.ec != std::errc{} || parsed.ptr != prob.data() + prob.size() || !(latency.prob >= 0) ||
      latency.prob > 1) {
    throw std::invalid_argument("--latency-prob must be a number in [0, 1]");
  }
  return latency;
}

int run_prepare(int argc, char** argv) {
  const auto options = parse_options(argc, argv, {"--input", "--output", "--shards"});
  const std::filesystem::path input = required(options, "--input");
  const std::filesystem::path output = required(options, "--output");
  const auto shards = parse_number<size_t>(required(options, "--shards"), "--shards", 1);
  kretrieve::prepare_corpus(input, output, shards);
  std::printf("Prepared %zu shards in %s\n", shards, output.c_str());
  return 0;
}

std::atomic<kretrieve::FrameServer*> g_server{nullptr};

void handle_stop_signal(int /*signal*/) {
  if (auto* server = g_server.load()) server->stop();
}

kretrieve::InvertedIndex load_index(const std::filesystem::path& stats_path,
                                    const std::filesystem::path& partition_path) {
  auto stats = kretrieve::read_global_stats(stats_path);
  std::ifstream partition(partition_path);
  if (!partition) throw std::runtime_error("cannot open partition: " + partition_path.string());
  const auto documents = kretrieve::load_corpus(partition, /*allow_empty=*/true);
  return kretrieve::InvertedIndex(documents, std::move(stats));  // document text is dropped here
}

// Serve frames with handler until SIGINT/SIGTERM. Reads --host, --port,
// --max-connections, --workers, --max-queue.
int serve(const kretrieve::RequestHandler& handler, const Options& options) {
  const auto port = parse_number<uint16_t>(required(options, "--port"), "--port", 0);
  const auto host = optional(options, "--host", "127.0.0.1");
  auto listener = kretrieve::listen_tcp(host, port);
  const auto bound_port = kretrieve::local_port(listener.get());
  kretrieve::FrameServer server(handler, std::move(listener), max_connections(options),
                                workers(options), max_queue(options));

  g_server = &server;
  struct sigaction action {};
  action.sa_handler = handle_stop_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  std::printf("listening on %s:%u\n", host.c_str(), unsigned{bound_port});
  std::fflush(stdout);
  try {
    server.run();
  } catch (...) {
    g_server = nullptr;
    throw;
  }
  g_server = nullptr;
  return 0;
}

int run_shard(int argc, char** argv) {
  const auto options =
      parse_options(argc, argv,
                    {"--stats", "--partition", "--port", "--host", "--max-connections", "--workers",
                     "--max-queue", "--latency-ms", "--latency-prob"});
  const auto stats = required(options, "--stats");
  const auto partition = required(options, "--partition");
  // Validate the remaining options before the slow index build.
  parse_number<uint16_t>(required(options, "--port"), "--port", 0);
  max_connections(options);
  workers(options);
  max_queue(options);
  const auto latency = injected_latency(options);

  const auto index = load_index(stats, partition);
  return serve(
      [&index, latency](const kretrieve::Frame& request, kretrieve::Clock::time_point received) {
        if (latency) {
          // Blocks this worker on purpose, like a slow disk would.
          thread_local std::mt19937 rng{std::random_device{}()};
          if (std::bernoulli_distribution(latency->prob)(rng)) {
            std::uniform_int_distribution<uint32_t> ms(latency->min_ms, latency->max_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms(rng)));
          }
        }
        return kretrieve::handle_request(index, request, received);
      },
      options);
}

int run_coordinator(int argc, char** argv) {
  const auto options = parse_options(
      argc, argv,
      {"--shards", "--port", "--host", "--max-connections", "--workers", "--max-queue"});
  const auto shards = kretrieve::parse_shard_list(required(options, "--shards"));
  // Same default as FrameServer: min(connection cap, CPU count) workers serve queries at once.
  const size_t concurrent =
      workers(options) != 0 ? workers(options)
                            : std::min(max_connections(options),
                                       size_t{std::max(1u, std::thread::hardware_concurrency())});
  // One fan-out thread per shard per concurrently served query.
  kretrieve::Coordinator coordinator(shards, shards.size() * concurrent);
  return serve(
      [&coordinator](const kretrieve::Frame& request, kretrieve::Clock::time_point received) {
        return coordinator.handle(request, received);
      },
      options);
}

int run_query(int argc, char** argv) {
  const auto options =
      parse_options(argc, argv, {"--host", "--port", "--k", "--query", "--budget-us"});
  const auto port = parse_number<uint16_t>(required(options, "--port"), "--port", 1);
  kretrieve::QueryRequest request;
  request.query = required(options, "--query");
  request.k = parse_number<uint32_t>(optional(options, "--k", "10"), "--k", 0);
  request.budget_us =
      parse_number<uint64_t>(optional(options, "--budget-us", "0"), "--budget-us", 0);

  const auto connection = kretrieve::connect_tcp(optional(options, "--host", "127.0.0.1"), port);
  const kretrieve::Frame frame{kretrieve::MsgType::QueryRequest, 1,
                               kretrieve::encode_request(request)};
  if (!kretrieve::send_all(connection.get(), kretrieve::encode_frame(frame))) {
    throw std::runtime_error("failed to send request");
  }
  const auto reply = kretrieve::read_frame(connection.get());
  if (!reply || reply->type != kretrieve::MsgType::QueryResponse || reply->req_id != frame.req_id) {
    throw std::runtime_error("no valid response from shard");
  }
  const auto response = kretrieve::decode_response(reply->body);
  if (!response) throw std::runtime_error("malformed response body");
  if (response->status != kretrieve::ResponseStatus::Ok) {
    static constexpr const char* kNames[] = {"ok", "bad request", "internal error", "overloaded"};
    std::fprintf(stderr, "query: server returned status %u (%s)\n",
                 static_cast<unsigned>(response->status),
                 kNames[static_cast<size_t>(response->status)]);
    return 1;
  }
  if (!response->missing_shards.empty()) {
    std::string missing;
    for (const auto shard : response->missing_shards) {
      missing += (missing.empty() ? "" : ",") + std::to_string(shard);
    }
    std::fprintf(stderr, "incomplete: missing shards %s\n", missing.c_str());
  }
  for (size_t i = 0; i < response->hits.size(); ++i) {
    std::printf("%zu\t%llu\t%.12f\n", i + 1,
                static_cast<unsigned long long>(response->hits[i].document_id),
                response->hits[i].score);
  }
  return 0;
}

int run_loadgen(int argc, char** argv) {
  const auto options = parse_options(argc, argv,
                                     {"--host", "--port", "--qps", "--duration", "--queries",
                                      "--out", "--k", "--budget-us", "--connections"});
  kretrieve::LoadConfig config;
  config.host = optional(options, "--host", "127.0.0.1");
  config.port = parse_number<uint16_t>(required(options, "--port"), "--port", 1);
  config.qps = parse_number<uint32_t>(required(options, "--qps"), "--qps", 1);
  config.duration = std::chrono::seconds(
      parse_number<uint32_t>(required(options, "--duration"), "--duration", 1));
  config.k = parse_number<uint32_t>(optional(options, "--k", "10"), "--k", 0);
  config.budget_us =
      parse_number<uint64_t>(optional(options, "--budget-us", "0"), "--budget-us", 0);
  config.connections =
      parse_number<size_t>(optional(options, "--connections", "32"), "--connections", 1);

  const std::string queries_path = required(options, "--queries");
  std::ifstream queries(queries_path);
  if (!queries) throw std::runtime_error("cannot open queries: " + queries_path);
  for (std::string line; std::getline(queries, line);) {
    if (!line.empty()) config.queries.push_back(line);
  }
  if (config.queries.empty()) throw std::runtime_error("no queries in " + queries_path);
  const std::string out_path = required(options, "--out");
  std::ofstream out(out_path);
  if (!out) throw std::runtime_error("cannot write " + out_path);

  const auto samples = kretrieve::run_load(config);
  kretrieve::write_csv(out, samples);
  out.close();
  if (!out) throw std::runtime_error("failed writing " + out_path);
  std::fprintf(stderr, "loadgen: %s\n", kretrieve::summarize(samples).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage(argv[0]);
  const std::string_view role = argv[1];

  int (*run)(int, char**) = nullptr;
  if (role == "prepare") {
    run = run_prepare;
  } else if (role == "shard") {
    run = run_shard;
  } else if (role == "query") {
    run = run_query;
  } else if (role == "coordinator") {
    run = run_coordinator;
  } else if (role == "loadgen") {
    run = run_loadgen;
  } else {
    return usage(argv[0]);
  }

  try {
    return run(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
    return usage(argv[0]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.what());
    return 1;
  }
}
