#ifndef SWITCHBOARD_RESPONSE_VIEW_H_
#define SWITCHBOARD_RESPONSE_VIEW_H_

#include "pub_sub/dynamic_service_call.h"

#include <QWidget>

#include <chrono>

class QLabel;
class QVBoxLayout;

namespace switchboard
{

// What came back from a call, as fields rather than as JSON.
//
// A status line ("✔ Replied · 42 ms", "✖ No reply after 2000 ms"), an optional
// banner, and one tree per reply with field, value and type columns -- the type
// read from the reply's schema, so a UInt8 that reads 0 is visibly a UInt8.
//
// THE `ok` BANNER IS A CONVENTION, NOT A CONTRACT. Most responses in this tree
// carry `ok :Bool` plus `error :Text` (or `message`), and a reply that says
// ok=false is a failure a person must not miss among a dozen other fields -- so
// it gets a red banner with the reason. Nothing enforces the shape; a response
// without an `ok` field just shows its fields.
class ResponseView : public QWidget
{
    Q_OBJECT

  public:
    explicit ResponseView(QWidget* parent = nullptr);

    // Nothing to show; `message` says why, if anything.
    void clear(const QString& message = {});

    void showPending(const QString& key);

    // `note` is shown under the status -- "from history 12:01:03".
    void showResult(const pub_sub::ServiceCallResult& result, const QString& note = {});

    // For tests and the agent interface: what a person would read.
    QString statusText() const;
    QString bannerText() const;
    int replySectionCount() const;

  private:
    QLabel* status_ = nullptr;
    QLabel* note_ = nullptr;
    QLabel* banner_ = nullptr;
    QWidget* sections_ = nullptr;
    QVBoxLayout* sections_layout_ = nullptr;
    int section_count_ = 0;
};

}  // namespace switchboard

#endif  // SWITCHBOARD_RESPONSE_VIEW_H_
