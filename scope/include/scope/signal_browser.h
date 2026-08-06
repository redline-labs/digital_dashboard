#ifndef SCOPE_SIGNAL_BROWSER_H_
#define SCOPE_SIGNAL_BROWSER_H_

#include "scope/panel.h"

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QLineEdit;
class QPushButton;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace scope
{

class DataSource;

// The mime type a dragged candidate travels as. Panels check for it before
// accepting a drop, and the payload is a BindingCandidate as JSON.
inline constexpr const char* kSignalMimeType = "application/x-redline-signal";

QByteArray encodeCandidate(const BindingCandidate& candidate);
bool decodeCandidate(const QByteArray& data, BindingCandidate& out);

// What is on the bus, as a topic -> field tree you can drag out of.
//
// DISCOVERY IS OBSERVATION, NOT QUERY, and the UI has to say so. zenoh has no
// retained messages, so the only way to know a topic exists is to see a sample
// of it: pub_sub::observeTopics subscribes for a window and reports what
// arrived. An empty result therefore means "nothing published in the last N
// seconds", never "nothing exists", and a browser that renders those the same
// way tells the user the bus is dead when it is merely idle.
//
// For the same reason a rescan MERGES into the tree rather than replacing it.
// A topic published every ten seconds would otherwise flicker in and out of the
// list depending on whether its sample happened to land inside the window.
class SignalBrowser : public QWidget
{
    Q_OBJECT

  public:
    explicit SignalBrowser(DataSource& source, QWidget* parent = nullptr);
    ~SignalBrowser() override;

    // Subscribe for `window_ms` and merge what arrives into the tree. Blocks
    // for that long, so it is called from a button or a timer, never from a
    // paint.
    void rescan(int window_ms = 1200);

    // Everything currently known, flattened: one entry per topic plus one per
    // field. This is what the agent interface's `scope.browser` reports, and it
    // is deliberately the same data the tree renders rather than a second
    // discovery path that could disagree with it.
    std::vector<BindingCandidate> candidates() const;

    // The candidate under a given topic/field, or false when the browser has
    // not seen it. Used by the agent interface so a caller can name a signal
    // without having to construct a candidate by hand.
    bool findCandidate(const QString& zenoh_key,
                       const QString& field_name,
                       BindingCandidate& out) const;

  signals:
    // Emitted when the user double-clicks or activates a candidate: the window
    // routes it to the focused panel, which is the keyboard path to the same
    // thing dragging does.
    void candidateActivated(const BindingCandidate& candidate);

  private:
    void applyFilter();
    void addTopic(const QString& key, const QString& schema, double hz);
    void onItemActivated(QTreeWidgetItem* item, int column);

    DataSource& source_;

    QLineEdit* filter_ = nullptr;
    QPushButton* rescan_button_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;

    bool ever_scanned_ = false;
};

}  // namespace scope

#endif  // SCOPE_SIGNAL_BROWSER_H_
