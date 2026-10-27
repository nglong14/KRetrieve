// Command-line entry point and role selection.
#include "index.hpp"
#include "net.hpp"
#include "prepare.hpp"
#include "protocol.hpp"
#include "shard_server.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s <role> [args]\n"
               "roles:\n"
               "  prepare --input <corpus.tsv> --output <new-directory> --shards <N>\n"
               "  shard --stats <global_stats.bin> --partition <shard-N.tsv> --port <P>\n"
               "        [--host 127.0.0.1] [--max-connections 64] [--workers <N>]\n"
               "  query --port <P> --query <text> [--host 127.0.0.1] [--k 10] [--budget-us 0]\n"
               "  coordinator  fan queries out to shards\n",
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

int run_prepare(int argc, char** argv) {
  const auto options = parse_options(argc, argv, {"--input", "--output", "--shards"});
  const std::filesystem::path input = required(options, "--input");
  const std::filesystem::path output = required(options, "--output");
  const auto shards = parse_number<size_t>(required(options, "--shards"), "--shards", 1);
  kretrieve::prepare_corpus(input, output, shards);
  std::printf("Prepared %zu shards in %s\n", shards, output.c_str());
  return 0;
}

std::atomic<kretrieve::ShardServer*> g_server{nullptr};

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

int run_shard(int argc, char** argv) {
  const auto options = parse_options(
      argc, argv, {"--stats", "--partition", "--port", "--host", "--max-connections", "--workers"});
  const auto stats = required(options, "--stats");
  const auto partition = required(options, "--partition");
  const auto port = parse_number<uint16_t>(required(options, "--port"), "--port", 0);
  const auto host = optional(options, "--host", "127.0.0.1");
  const auto max_connections =
      parse_number<size_t>(optional(options, "--max-connections", "64"), "--max-connections", 1);
  const auto workers = options.contains("--workers")
                           ? parse_number<size_t>(required(options, "--workers"), "--workers", 1)
                           : 0;

  const auto index = load_index(stats, partition);
  auto listener = kretrieve::listen_tcp(host, port);
  const auto bound_port = kretrieve::local_port(listener.get());
  kretrieve::ShardServer server(index, std::move(listener), max_connections, workers);

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
    std::fprintf(stderr, "query: shard returned status %u\n",
                 static_cast<unsigned>(response->status));
    return 1;
  }
  for (size_t i = 0; i < response->hits.size(); ++i) {
    std::printf("%zu\t%llu\t%.12f\n", i + 1,
                static_cast<unsigned long long>(response->hits[i].document_id),
                response->hits[i].score);
  }
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
    std::fprintf(stderr, "coordinator: not implemented yet (plan day 3)\n");
    return 1;
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
