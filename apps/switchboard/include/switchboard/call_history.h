#ifndef SWITCHBOARD_CALL_HISTORY_H_
#define SWITCHBOARD_CALL_HISTORY_H_

#include "pub_sub/dynamic_service_call.h"

#include <QDateTime>
#include <QWidget>

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

class QLabel;
class QListWidget;

namespace switchboard
{

using json = nlohmann::json;

// One call: what was asked, of whom, and -- once zenoh says the query is over --
// what came back.
struct CallRecord
{
    int call_id = 0;
    QDateTime started;

    std::string key;
    std::string owner_zid;
    std::string request_schema;
    std::string response_schema;
    json request = json::object();
    std::chrono::milliseconds timeout{0};

    bool complete = false;
    pub_sub::ServiceCallResult result;
};

// The result as the agent interface and "Copy as JSON" spell it.
json resultToJson(const pub_sub::ServiceCallResult& result);
json recordToJson(const CallRecord& record);

// "12:01:03  ✔ 42 ms", "12:00:51  ✖ no reply" -- one line for the history list.
QString summarize(const CallRecord& record);

// The calls made this session, per service key, newest first.
//
// In memory only, on purpose: a request log that outlived the session would
// replay yesterday's "set the bitrate" into today's car at a click. Per key
// rather than per offering node, because a call to a key reaches every node
// offering it -- the node was never a choice the caller made.
class CallHistory : public QWidget
{
    Q_OBJECT

  public:
    // Per key. Enough to scroll back through an afternoon of poking one service
    // without the list becoming a log.
    static constexpr std::size_t kPerKey = 50;

    explicit CallHistory(QWidget* parent = nullptr);

    void add(CallRecord record);

    // Fills in a record's result. Returns it, or null for an id no longer kept
    // (a pending call pushed out by fifty newer ones).
    const CallRecord* complete(int call_id, pub_sub::ServiceCallResult result);

    const CallRecord* find(int call_id) const;

    // Newest first. Empty key means every key, newest first.
    std::vector<const CallRecord*> forKey(const std::string& key) const;

    // Which key the list shows.
    void showKey(const std::string& key);

  signals:
    // The user picked a past call to look at again.
    void recordActivated(int call_id);

  private:
    void rebuild();

    std::map<std::string, std::deque<CallRecord>> records_;
    std::string shown_key_;
    QListWidget* list_ = nullptr;
    QLabel* empty_ = nullptr;
};

}  // namespace switchboard

#endif  // SWITCHBOARD_CALL_HISTORY_H_
