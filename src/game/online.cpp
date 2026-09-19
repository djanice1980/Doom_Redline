#include "game/online.h"

#include <cstdio>

namespace rl::game {

OnlineClient::OnlineClient() { thread_ = std::thread([this] { worker(); }); }

OnlineClient::~OnlineClient() {
    { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void OnlineClient::setServer(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    std::lock_guard<std::mutex> lock(mutex_);
    server_ = std::move(url);
}

int OnlineClient::enqueue(Kind kind, std::string tag, std::function<net::HttpResponse()> run) {
    int id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = nextId_++;
        jobs_.push_back({kind, id, std::move(tag), std::move(run)});
    }
    cv_.notify_one();
    return id;
}

void OnlineClient::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }
        net::HttpResponse r = job.run();
        Result res;
        res.kind = job.kind;
        res.id = job.id;
        res.tag = job.tag;
        res.status = r.status;
        if (!r.ok) res.error = r.error;
        else {
            std::string perr;
            if (!r.body.empty() && !Json::parse(r.body, res.body, &perr)) res.error = "bad reply: " + perr;
            else if (r.status >= 200 && r.status < 300) res.ok = true;
            else res.error = res.body["error"].asString(res.body["message"].asString("HTTP " + std::to_string(r.status)));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            results_.push_back(std::move(res));
            --running_;
        }
    }
}

int OnlineClient::registerEmail(const std::string& playerId, const std::string& installId, const std::string& email, const std::string& displayName, const std::string& machineLabel, const std::string& version) {
    Json body = Json::object();
    body.set("player_id", playerId).set("install_id", installId).set("email", email).set("display_name", displayName).set("machine_label", machineLabel).set("version", version);
    const std::string url = server_ + "/api/register", text = body.dump();
    return enqueue(Kind::Register, "", [url, text] { return net::httpPost(url, text, {"Content-Type: application/json"}); });
}

int OnlineClient::confirm(const std::string& playerId, const std::string& code) {
    Json body = Json::object();
    body.set("player_id", playerId).set("code", code);
    const std::string url = server_ + "/api/confirm", text = body.dump();
    return enqueue(Kind::Confirm, "", [url, text] { return net::httpPost(url, text, {"Content-Type: application/json"}); });
}

int OnlineClient::submitRun(const std::string& runJson, const std::string& token, const std::string& tag) {
    const std::string url = server_ + "/api/runs";
    const std::vector<std::string> headers = {"Content-Type: application/json", "Authorization: Bearer " + token};
    return enqueue(Kind::SubmitRun, tag, [url, runJson, headers] { return net::httpPost(url, runJson, headers); });
}

int OnlineClient::leaderboard(const std::string& board, const std::string& playerId) {
    const std::string url = server_ + "/api/leaderboard?board=" + board + (playerId.empty() ? "" : "&player=" + playerId);
    return enqueue(Kind::Leaderboard, board, [url] { return net::httpGet(url); });
}

int OnlineClient::player(const std::string& playerId) {
    const std::string url = server_ + "/api/player/" + playerId;
    return enqueue(Kind::Player, playerId, [url] { return net::httpGet(url); });
}

int OnlineClient::version() {
    const std::string url = server_ + "/api/version";
    return enqueue(Kind::Version, "", [url] { return net::httpGet(url); });
}

int OnlineClient::pollRegistration(const std::string& playerId, const std::string& pollSecret) {
    Json body = Json::object();
    body.set("player_id", playerId).set("poll_secret", pollSecret);
    const std::string url = server_ + "/api/registration", text = body.dump();
    return enqueue(Kind::Poll, "", [url, text] { return net::httpPost(url, text, {"Content-Type: application/json"}); });
}

std::vector<OnlineClient::Result> OnlineClient::poll() {
    std::vector<Result> out;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!results_.empty()) { out.push_back(std::move(results_.front())); results_.pop_front(); }
    return out;
}

bool OnlineClient::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ > 0 || !jobs_.empty();
}

}  // namespace rl::game
