#include "position.hpp"
#include "nn/evaluator.hpp"

enum SearchResultType { kSearchResultInfo, kSearchResultBestMove, kNoSearchResult };

struct SearchResult {
  SearchResultType type = kNoSearchResult;
  int depth = 0;
  Score score = 0;
  vector<Move> pv;

  void print(std::ostream& ostr = std::cerr) const { ostr << make_tuple(type, depth, score, pv); }
  friend std::ostream& operator<<(std::ostream& ostr, const SearchResult& self) { self.print(ostr); return ostr; }
};


struct GoParameters {
  // Time unit is millisecond
  array<int64_t, 2> time = {0, 0};
  array<int64_t, 2> inc = {0, 0};
  int64_t movestogo = 1;
  int64_t movetime = 0;
  int depth = Position::kMaxDepth;
};


struct TimeControl {
  using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
  static inline TimePoint now() { return std::chrono::steady_clock::now(); }
  static inline const double kSafeFactor = 0.8;
  static inline const double kInfDuration = 1e12; // 10^12 msec ~ 30 years

  TimePoint start;
  TimePoint finish = now() + std::chrono::milliseconds(int64_t(kInfDuration));

  void initialize(const GoParameters& go, Color own) {
    start = now();
    double duration = kInfDuration;
    if (go.movetime != 0) {
      duration = std::min(duration, (double)go.movetime);
    }
    if (go.time[own] != 0) {
      double time = go.time[own];
      double inc = go.inc[own];
      double cnt = go.movestogo;
      duration = std::min(duration, (time + inc * (cnt - 1)) / cnt);
    }
    finish = start + std::chrono::milliseconds(int64_t(kSafeFactor * duration));
  }

  bool checkLimit() { return now() < finish; }
};


struct SearchState {
  array<Move, Position::kMaxDepth> pv;
};


struct Engine {
  Position position;
  nn::Evaluator evaluator;

  GoParameters go_parameters = {};
  TimeControl time_control = {};

  std::atomic<bool> stop_requested = 0; // single reader ("go" thread) + single writer ("stop" thread)

  // Engine::wait invalidates future for the next Engine::go
  std::future<bool> go_thread_future;
  bool isRunning() { return go_thread_future.valid(); }

  // Result for each depth during iterative deepening
  vector<SearchResult> results;
  std::function<void(const SearchResult&)> search_result_callback = [](auto){};

  SearchState* search_state = nullptr;
  array<SearchState, Position::kMaxDepth> search_state_stack;

  Engine() {
    evaluator.loadEmbeddedWeight();
    position.evaluator = &evaluator;
    position.reset();
  }

  // "go" and "stop/wait" should be called from different threads
  void stop();
  void wait();

  // Iterative deepening
  void go(bool blocking);
  void goImpl();
  bool checkSearchLimit();

  // Fixed depth alph-beta search (NOTE: Only usable from "go" method)
  SearchResult search(int);
  Score searchImpl(Score, Score, int, int);

  void load(const string& filename) { evaluator.load(filename); }

  void print(std::ostream&);
};
