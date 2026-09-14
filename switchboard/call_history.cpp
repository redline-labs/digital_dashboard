#include "switchboard/call_history.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace switchboard
{

namespace
{

constexpr int kCallIdRole = Qt::UserRole + 1;

}  // namespace

json resultToJson(const pub_sub::ServiceCallResult& result)
{
    json replies = json::array();
    for (const pub_sub::ServiceReply& reply : result.replies)
    {
        json item = json::object();
        item["is_error"] = reply.is_error;
        if (reply.is_error)
        {
            item["error_text"] = reply.error_text;
        }
        item["schema"] = reply.schema;
        item["value"] = reply.value;
        replies.push_back(std::move(item));
    }

    json out = json::object();
    out["status"] = pub_sub::to_string(result.status);
    out["elapsed_ms"] = result.elapsed.count();
    out["errors"] = result.errors;
    out["replies"] = std::move(replies);
    return out;
}

json recordToJson(const CallRecord& record)
{
    json out = json::object();
    out["call_id"] = record.call_id;
    out["started"] = record.started.toString(Qt::ISODateWithMs).toStdString();
    out["key"] = record.key;
    out["owner_zid"] = record.owner_zid;
    out["request_schema"] = record.request_schema;
    out["response_schema"] = record.response_schema;
    out["request"] = record.request;
    out["timeout_ms"] = record.timeout.count();
    out["pending"] = !record.complete;
    if (record.complete)
    {
        out["result"] = resultToJson(record.result);
    }
    return out;
}

QString summarize(const CallRecord& record)
{
    const QString when = record.started.toString("HH:mm:ss");
    if (!record.complete)
    {
        return when + "  … calling";
    }

    using Status = pub_sub::ServiceCallResult::Status;
    const auto& result = record.result;
    const QString elapsed = QString("%1 ms").arg(result.elapsed.count());

    switch (result.status)
    {
        case Status::Replied:
        {
            const bool any_error =
                std::any_of(result.replies.begin(), result.replies.end(),
                            [](const pub_sub::ServiceReply& reply) { return reply.is_error; });
            if (any_error)
            {
                return when + "  ⚠ error · " + elapsed;
            }
            return when + "  ✔ " + elapsed +
                   (result.replies.size() > 1 ? QString(" · %1 replies").arg(result.replies.size())
                                              : QString());
        }
        case Status::NoReply:
            return when + "  ✖ no reply";
        case Status::RequestRejected:
            return when + "  ✖ rejected";
        case Status::Failed:
            return when + "  ✖ failed";
    }
    return when;
}

CallHistory::CallHistory(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel("History");
    title->setStyleSheet("font-weight: 700; font-size: 13px;");
    layout->addWidget(title);

    empty_ = new QLabel("No calls to this service yet.");
    empty_->setStyleSheet("color: palette(mid); font-size: 11px;");
    layout->addWidget(empty_);

    list_ = new QListWidget();
    list_->setObjectName("history_list");
    list_->setToolTip("Click a past call to put its request back in the form and show its result.");
    layout->addWidget(list_, 1);

    connect(list_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) { emit recordActivated(item->data(kCallIdRole).toInt()); });

    rebuild();
}

void CallHistory::add(CallRecord record)
{
    auto& list = records_[record.key];
    list.push_front(std::move(record));
    while (list.size() > kPerKey)
    {
        list.pop_back();
    }
    rebuild();
}

const CallRecord* CallHistory::complete(int call_id, pub_sub::ServiceCallResult result)
{
    for (auto& [key, list] : records_)
    {
        for (CallRecord& record : list)
        {
            if (record.call_id == call_id)
            {
                record.result = std::move(result);
                record.complete = true;
                rebuild();
                return &record;
            }
        }
    }
    return nullptr;
}

const CallRecord* CallHistory::find(int call_id) const
{
    for (const auto& [key, list] : records_)
    {
        for (const CallRecord& record : list)
        {
            if (record.call_id == call_id)
            {
                return &record;
            }
        }
    }
    return nullptr;
}

std::vector<const CallRecord*> CallHistory::forKey(const std::string& key) const
{
    std::vector<const CallRecord*> out;
    for (const auto& [record_key, list] : records_)
    {
        if (!key.empty() && record_key != key)
        {
            continue;
        }
        for (const CallRecord& record : list)
        {
            out.push_back(&record);
        }
    }
    // Ids are handed out in order, so newest first is highest id first.
    std::sort(out.begin(), out.end(),
              [](const CallRecord* a, const CallRecord* b) { return a->call_id > b->call_id; });
    return out;
}

void CallHistory::showKey(const std::string& key)
{
    shown_key_ = key;
    rebuild();
}

void CallHistory::rebuild()
{
    list_->clear();

    const auto it = records_.find(shown_key_);
    if (it != records_.end())
    {
        for (const CallRecord& record : it->second)
        {
            auto* item = new QListWidgetItem(summarize(record));
            item->setData(kCallIdRole, record.call_id);
            item->setToolTip(QString::fromStdString(record.request.dump(2)));
            list_->addItem(item);
        }
    }

    empty_->setVisible(list_->count() == 0);
    list_->setVisible(list_->count() != 0);
}

}  // namespace switchboard
