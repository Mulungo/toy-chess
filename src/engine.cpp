#include "engine.hpp"
#include "move_picker.hpp"

void TimeControl::initialize(const GoParameters& go, Color own, int ply) {
  start = now();
  double duration = kInfDuration;
  if (go.movetime != 0) {
    duration = std::min(duration, (double)go.movetime);
  }
  if (go.time[own] != 0) {
    double time = go.time[own];
    double inc = go.inc[own];
    int cnt = go.movestogo ? go.movestogo : std::max(10, 32 - ply / 2);
    duration = std::min(duration, (time + inc * (cnt - 1)) / cnt); // Split remaining time to each move

    if (ply <= 8) {
      double opening_time = 1000.0 + (1000. / 8.) * ply;
      duration = std::min(duration, opening_time);
    }
  }
  finish = start + Msec(int64_t(kSafeFactor * duration));
};

void SearchResult::print(std::ostream& ostr) const {
  if (type == kSearchResultInfo) {
    if (!debug.empty()) {
      ostr << "info string " << debug;

    } else {
      int64_t nps = (1000 * stats_nodes) / stats_time;
      ostr << "info"
           << " depth " << depth
           << " score cp " << score
           << " time " << stats_time
           << " nodes " << stats_nodes
           << " nps " << nps
           << " pv";
      for (auto move : pv) { ostr << " " << move; }
    }
  }

  if (type == kSearchResultBestMove) {
    ostr << "bestmove " << pv.data[0];
  }
}

void Engine::print(std::ostream& ostr) {
  ostr << ":: Position" << "\n";
  ostr << position;
  ostr << ":: Evaluation" << "\n";
  ostr << evaluator.evaluate() << "\n";
}

void Engine::stop() {
  if (!isRunning()) { return; }
  ASSERT(!stop_requested.load(std::memory_order_acquire));
  stop_requested.store(true, std::memory_order_release);
  wait();
  stop_requested.store(false, std::memory_order_release);
}

void Engine::wait() {
  ASSERT(go_thread_future.valid()); // Check Engine::go didn't meet with Engine::wait yet
  go_thread_future.wait();
  ASSERT(go_thread_future.get());
  go_thread_future = {}; // Invalidate future
}

bool Engine::checkSearchLimit() {
  if (stop_requested.load(std::memory_order_acquire)) { return 0; }
  if (!time_control.checkLimit()) { return 0; }
  return 1;
}

void Engine::go(bool blocking) {
  ASSERT(!go_thread_future.valid()); // Check previous Engine::go met with Engine::wait
  go_thread_future = std::async([&]() { goImpl(); return true; });
  if (blocking) { wait(); }
}

void Engine::goImpl() {
  time_control.initialize(go_parameters, position.side_to_move, position.game_ply);
  // TODO: When no increment, this might fail (essentiall we got flagged...)
  ASSERT(checkSearchLimit());

  // Debug info
  SearchResult info;
  info.type = kSearchResultInfo;
  info.debug += "ply = " + toString(position.game_ply) + ", ";
  info.debug += "side = " + toString(position.side_to_move) + ", ";
  info.debug += "eval = " + toString(position.evaluate()) + ", ";
  info.debug += "time = " + toString(time_control.getDuration());
  search_result_callback(info);

  int depth_end = go_parameters.depth;
  ASSERT(depth_end > 0);
  results.assign(depth_end + 1, {});

  // Construct result for depth = 1 so that there always is "bestmove" result.
  int last_depth = 1;
  results[1] = search(1);
  results[1].type = kSearchResultInfo;
  search_result_callback(results[1]);

  // Iterative deepening
  for (int depth = 2; depth <= depth_end; depth++) {
    SearchResult res;
    if (depth < 4) {
      res = search(depth);
    } else {
      res = searchWithAspirationWindow(depth, results[depth - 1].score);
    }
    if (!checkSearchLimit()) { break; } // Ignore possibly incomplete result

    // Save result and send "info ..."
    last_depth = depth;
    results[depth] = res;
    results[depth].type = kSearchResultInfo;
    results.push_back(results[depth]);
    search_result_callback(results[depth]);

    // Debug info
    SearchResult res_info;
    res_info.type = kSearchResultInfo;
    res_info.debug += "tt_hit = " + toString(res.stats_tt_hit) + ", ";
    res_info.debug += "tt_cut = " + toString(res.stats_tt_cut) + ", ";
    res_info.debug += "null_prune = " + toString(res.stats_null_prune_success) + "/" + toString(res.stats_null_prune) + ", ";
    res_info.debug += "futility_prune = " + toString(res.stats_futility_prune) + ", ";
    res_info.debug += "lmr = " + toString(res.stats_lmr_success) + "/" + toString(res.stats_lmr);
    search_result_callback(res_info);
  }

  // Send "bestmove ..."
  results[last_depth].type = kSearchResultBestMove;
  search_result_callback(results[last_depth]);
}

SearchResult Engine::searchWithAspirationWindow(int depth, Score init_target) {
  const Score kInitDelta = 25;

  // Prevent overflow
  int32_t delta = kInitDelta;
  int32_t target = init_target;
  int32_t kInf = kScoreInf;
  int32_t alpha, beta;

  while (true) {
    alpha = std::max(target - delta, -kInf);
    beta = std::min(target + delta, kInf);

    SearchResult res;
    res.depth = depth;

    state = &search_state_stack[0];
    Score score = searchImpl(alpha, beta, 0, depth, res);
    if (!checkSearchLimit()) { return {}; }

    if (alpha < score && score < beta) {
      res.score = score;
      res.pv = state->pv;
      res.stats_time = time_control.getTime() + 1;
      return res;
    }

    // Extend low
    //       <--t-->
    // <-----t----->
    if (score <= alpha) { target -= delta; }

    // Extend high
    // <--t-->
    // <-----t----->
    if (beta <= alpha) { target += delta; }

    delta *= 2;
  }

  ASSERT(0);
  return {};
}

SearchResult Engine::search(int depth) {
  SearchResult res;
  res.depth = depth;

  state = &search_state_stack[0];
  res.score = searchImpl(-kScoreInf, kScoreInf, 0, depth, res);
  res.pv = state->pv;
  res.stats_time = time_control.getTime() + 1;
  return res;
}

Score Engine::searchImpl(Score alpha, Score beta, int depth, int depth_end, SearchResult& result) {
  if (!checkSearchLimit()) { return kScoreNone; }
  if (depth >= depth_end) { return quiescenceSearch(alpha, beta, depth, result); }

  result.stats_nodes++;

  TTEntry tt_entry;
  bool tt_hit = transposition_table.get(position.state->key, tt_entry);
  result.stats_tt_hit += tt_hit;

  Move best_move = kNoneMove;
  NodeType node_type = kAllNode;
  Score score = -kScoreInf;
  Score evaluation = kScoreNone;

  bool interrupted = 0;
  int depth_to_go = depth_end - depth;
  bool in_check = position.state->checkers;
  Move tt_move = tt_hit ? tt_entry.move : kNoneMove;
  MoveList searched_quiets, searched_captures;
  int move_cnt = 0;

  // Use lambda to skip from anywhere to the end
  ([&]() {

    if (tt_hit) {
      // Hash score cut
      if (depth_to_go <= tt_entry.depth) {
        if (beta <= tt_entry.score && (tt_entry.node_type == kCutNode || tt_entry.node_type == kPVNode)) {
          score = tt_entry.score;
          node_type = kCutNode;
          best_move = tt_move;
          result.stats_tt_cut++;
          return;
        }
        if (tt_entry.score <= alpha && tt_entry.node_type == kAllNode) {
          score = tt_entry.score;
          node_type = kAllNode;
          result.stats_tt_cut++;
          return;
        }
      }

      // Hash evaluation
      evaluation = tt_entry.evaluation;
    }

    // Static evaluation
    if (evaluation == kScoreNone) { evaluation = position.evaluate(); }

    MovePicker move_picker(position, history, tt_move, state->killers, in_check, /* quiescence */ false);
    Move move;
    while (move_picker.getNext(move)) {
      move_cnt++;

      bool is_capture = position.isCaptureOrPromotion(move);
      (is_capture ? searched_captures : searched_quiets).put(move);

      makeMove(move);
      score = std::max<Score>(score, -searchImpl(-beta, -alpha, depth + 1, depth_end, result));
      unmakeMove(move);
      if (!checkSearchLimit()) { interrupted = 1; return; }

      if (beta <= score) { // beta cut
        node_type = kCutNode;
        best_move = move;
        return;
      }
      if (alpha < score) { // pv
        node_type = kPVNode;
        alpha = score;
        best_move = move;
        state->updatePV(move, (state + 1)->pv);
      }
    }

    // Checkmate/stalemate
    if (move_cnt == 0) { score = std::max<Score>(score, position.evaluateLeaf(depth)); }

  })(); // Lambda End

  if (interrupted) { return kScoreNone; }

  ASSERT(-kScoreInf < score && score < kScoreInf);
  tt_entry.node_type = node_type;
  tt_entry.move = best_move;
  tt_entry.score = score;
  tt_entry.evaluation = evaluation;
  tt_entry.depth = depth_to_go;
  transposition_table.put(position.state->key, tt_entry);

  if (node_type == kCutNode) {
    updateKiller(best_move);
    updateHistory(best_move, searched_quiets, searched_captures, depth_to_go);
  }

  return score;
}

Score Engine::quiescenceSearch(Score alpha, Score beta, int depth, SearchResult& result) {
  if (!checkSearchLimit()) { return kScoreNone; }

  result.stats_nodes++;
  if (depth >= Position::kMaxDepth) { return position.evaluate(); }

  TTEntry tt_entry;
  bool tt_hit = transposition_table.get(position.state->key, tt_entry);
  result.stats_tt_hit += tt_hit;

  Move best_move = kNoneMove;
  NodeType node_type = kAllNode;
  Score score = -kScoreInf;
  Score evaluation = kScoreNone;

  bool interrupted = 0;
  bool in_check = position.state->checkers;
  Move tt_move = tt_hit ? tt_entry.move : kNoneMove;
  int move_cnt = 0;

  ([&]() {

    if (tt_hit) {
      // Hash score cut
      if (tt_entry.node_type == kCutNode || tt_entry.node_type == kPVNode) {
        if (beta <= tt_entry.score) {
          score = tt_entry.score;
          node_type = kCutNode;
          best_move = tt_entry.move;
          result.stats_tt_cut++;
          return;
        }
      }
      if (tt_entry.node_type == kAllNode) {
        if (tt_entry.score <= alpha) {
          score = tt_entry.score;
          node_type = kAllNode;
          result.stats_tt_cut++;
          return;
        }
      }

      // Hash evaluation
      evaluation = tt_entry.evaluation;
    }

    // Static evaluation
    if (evaluation == kScoreNone) { evaluation = position.evaluate(); }

    // Stand pat beta cut
    score = evaluation;
    if (beta <= score) { node_type = kCutNode; return; }
    if (alpha < score) { alpha = score; }

    MovePicker move_picker(position, history, tt_move, state->killers, in_check, /* quiescence */ true);
    Move move;
    while (move_picker.getNext(move)) {
      move_cnt++;
      makeMove(move);
      score = std::max<Score>(score, -quiescenceSearch(-beta, -alpha, depth + 1, result));
      unmakeMove(move);

      if (!checkSearchLimit()) { interrupted = 1; return; }

      if (beta < score) {
        node_type = kCutNode;
        best_move = move;
        return;
      }
      if (alpha < score) {
        node_type = kPVNode;
        alpha = score;
      }
    }

    // Checkmate/stalemate
    if (in_check && move_cnt == 0) { score = position.evaluateLeaf(depth); }

  })(); // Lambda End

  if (interrupted) { return kScoreNone; }

  ASSERT(-kScoreInf < score && score < kScoreInf);
  tt_entry.node_type = node_type;
  tt_entry.move = best_move;
  tt_entry.score = score;
  tt_entry.evaluation = evaluation;
  tt_entry.depth = 0;
  transposition_table.put(position.state->key, tt_entry);

  return score;
}

void Engine::updateKiller(const Move& move) {
  auto& [m0, m1] = state->killers;
  if (m0 == move) { return; }
  m1 = move;
  std::swap(m0, m1);
}

void Engine::updateHistory(const Move& best_move, const MoveList& quiets, const MoveList& captures, int depth) {
  const Score kMaxHistoryScore = 2000;

  auto update = [&](Score sign, Score& result) {
    result += sign * (depth * depth);
    result = std::min<Score>(result, +kMaxHistoryScore);
    result = std::max<Score>(result, -kMaxHistoryScore);
  };

  if (position.isCaptureOrPromotion(best_move)) {
    // Increase best capture
    update(+1, history.getCaptureScore(position, best_move));

  } else {
    // Increase best quiet
    update(+1, history.getQuietScore(position, best_move));

    // Decrease all non-best quiets
    for (auto move : quiets) {
      if (move == best_move) { continue; }
      update(-1, history.getQuietScore(position, move));
    }
  }

  // Decrease all non-best captures
  for (auto move : captures) {
    if (move == best_move) { continue; }
    update(-1, history.getCaptureScore(position, move));
  }
}

void Engine::makeMove(const Move& move) {
  if (move == kNoneMove) {
    position.makeNullMove();
  } else {
    position.makeMove(move);
  }
  state++;
  state->reset();
}

void Engine::unmakeMove(const Move& move) {
  state--;
  if (move == kNoneMove) {
    position.unmakeNullMove();
  } else {
    position.unmakeMove(move);
  }
}
