#ifndef SCOPE_PANEL_H_
#define SCOPE_PANEL_H_

#include "scope/panel_types.h"

#include <QString>
#include <QWidget>

#include <string>

namespace scope
{

class DataSource;
class TimeBase;

// One thing the signal browser can offer a panel: either a whole topic, or one
// field of one.
//
// This is the type that makes a new panel kind cheap. The browser produces
// these, the drag carries them, the "Add signal..." dialog returns them, and
// every panel answers the same two questions about them -- will you take this,
// and here, take it. So a video panel arriving later needs no new browser, no
// new drag plumbing and no new dialog: it accepts a topic-level candidate whose
// schema is CarplayVideo, and rejects everything else.
//
// `field_name` empty means the candidate is the topic itself. A time-series
// panel wants a field; a video panel would want the topic.
struct BindingCandidate
{
    std::string zenoh_key;
    std::string schema_name;

    // Empty for a topic-level candidate.
    std::string field_name;

    // capnp's own category for the field: "int", "uint", "float", "bool",
    // "text", "data", "list", "struct", "enum", "void" or "other", exactly as
    // pub_sub::describeSchema() reports it. Empty for a topic-level candidate.
    //
    // A string rather than an enum on purpose: it comes from describeSchema()
    // and goes into the agent interface as JSON, so translating it into an enum
    // here would mean translating it back twice.
    std::string type_category;

    // The four capnp categories an expression can turn into a number. Anything
    // else is a config error the evaluator would reject at construction, so the
    // browser marks them and a plot declines them.
    bool isNumeric() const
    {
        return type_category == "int" || type_category == "uint" || type_category == "float" ||
               type_category == "bool";
    }

    bool isTopicLevel() const { return field_name.empty(); }
};

// What every panel is.
//
// Panels are QWidgets and live inside QDockWidgets; the dock is the chrome and
// this is the content. A panel does not own a timer -- TimeBase drives every
// panel from one -- and does not talk to the DataSource directly; the window
// binds signals on its behalf and hands it buffers.
class Panel : public QWidget
{
    Q_OBJECT

  public:
    using QWidget::QWidget;
    ~Panel() override = default;

    virtual panel_type_t panelType() const = 0;

    // Would this panel accept that? Asked by the drag's accept/reject logic and
    // by the agent interface before it does anything, so a rejection is a clean
    // "no" rather than a panel quietly ignoring a drop.
    virtual bool acceptsBinding(const BindingCandidate& candidate) const = 0;

    // Take it. False when the panel declined after all -- a duplicate, say.
    // Callers should check acceptsBinding() first; this returning false is not
    // an error, it is a panel exercising judgement about its own contents.
    virtual bool addBinding(const BindingCandidate& candidate) = 0;

    // The shared clock. Panels connect to its frame() to drain and repaint, and
    // read viewBegin()/viewEnd()/cursor() from it. Must outlive the panel.
    virtual void setTimeBase(TimeBase* time_base) = 0;

    // Move every binding onto a different source: the window has entered review
    // over a recording, or gone back to live.
    //
    // THE ORDERING RULE, and it is not optional. An implementation must release
    // its handles against the OLD source before it repoints, because a handle
    // means nothing to a source that did not issue it -- and the window only
    // destroys the old source after this returns, precisely so that release has
    // somewhere to go. Repointing first leaks every subscription on the old
    // source, which for the live one means it keeps decoding samples nothing
    // will ever draw.
    virtual void rebindTo(DataSource& source) = 0;

    // Seconds of samples each of this panel's signals retains. A workspace-level
    // setting rather than a per-panel one: two panels plotting the same signal
    // should not disagree about how far back it goes.
    //
    // Rebinds, which discards the history already collected. That is the honest
    // outcome -- a buffer cannot grow a past it never recorded -- and it is why
    // the window applies this before adding panels rather than after.
    virtual void setHistorySeconds(double seconds) = 0;

    // Human-readable, shown on the dock's title bar.
    virtual QString title() const = 0;

  signals:
    // The panel's configuration changed: a trace added or removed, a config
    // applied. The window listens so it knows the workspace no longer matches
    // what is on disk.
    void configChanged();
};

}  // namespace scope

#endif  // SCOPE_PANEL_H_
