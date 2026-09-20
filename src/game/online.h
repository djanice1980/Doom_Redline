#pragma once
// The game's side of the online service (docs/online-and-releases.md, sections
// 4, 7 and 9): registration by emailed code, run submission with a per-player
// token, leaderboard fetches. Every request runs on one worker thread; the
// game calls poll() each frame and handles the results on the main thread.
//
// Wire format (JSON, all routes under <server>/api):
//   POST register     {player_id, install_id, email, display_name, machine_label, version}
//                     -> {ok:true} (a code was emailed) | {error}
//   POST confirm      {player_id, code}  -> {ok:true, token} | {error}
//   POST runs         Authorization: Bearer <token>, body = RunRecord JSON
//                     -> {ok:true, rank, best_rank, total_players, tier} | {error}
//   GET  leaderboard?board=<name>&player=<id> -> {board, rows:[...], me:{...}|null, total}
//   POST registration {player_id, poll_secret} -> {status: pending|confirmed|declined|expired, token?}
//   GET  version -> {latest, url, published_at, changelog:[{version, date, items:[...]}]}
//   POST player/adopt  Authorization: Bearer <token>, {player_id: <the older player>}
//                     -> {ok:true, player_id, runs_moved} | {error}
//   POST player/delete Authorization: Bearer <token> -> {ok:true, runs_deleted} | {error}
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "net/http.h"

namespace rl::game {

class OnlineClient {
public:
    enum class Kind { Register, Confirm, SubmitRun, Leaderboard, Player, Version, Poll, Adopt, DeletePlayer };
    struct Result {
        Kind kind;
        int id = 0;
        bool ok = false;        // transport worked and the status was 2xx
        int status = 0;
        Json body;              // parsed reply (may be null)
        std::string error;      // transport error or the reply's "error" field
        std::string tag;        // caller data (the run file path for SubmitRun, the board name for Leaderboard)
    };

    OnlineClient();
    ~OnlineClient();
    OnlineClient(const OnlineClient&) = delete;
    OnlineClient& operator=(const OnlineClient&) = delete;

    void setServer(std::string url);   // "https://host" without a trailing slash
    const std::string& server() const { return server_; }
    static bool available() { return net::httpAvailable(); }

    int registerEmail(const std::string& playerId, const std::string& installId, const std::string& email, const std::string& displayName, const std::string& machineLabel, const std::string& version);
    int confirm(const std::string& playerId, const std::string& code);
    int submitRun(const std::string& runJson, const std::string& token, const std::string& tag);
    int leaderboard(const std::string& board, const std::string& playerId);
    int player(const std::string& playerId);
    int version();                                                     // GET /api/version: latest release + changelog
    int pollRegistration(const std::string& playerId, const std::string& pollSecret);   // POST /api/registration
    // This profile is really an older player of the same account: merge the two (the token stays valid).
    int adopt(const std::string& token, const std::string& oldPlayerId);
    // Erase this player from the service. `tag` carries the id, so a queued delete can be retired by name.
    int deletePlayer(const std::string& token, const std::string& tag);

    std::vector<Result> poll();   // completed results since the last call
    bool busy() const;            // requests still queued or running

private:
    struct Job { Kind kind; int id; std::string tag; std::function<net::HttpResponse()> run; };
    int enqueue(Kind kind, std::string tag, std::function<net::HttpResponse()> run);
    void worker();

    std::string server_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    int nextId_ = 1;
    int running_ = 0;
    bool quit_ = false;
};

}  // namespace rl::game
