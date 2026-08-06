#ifndef SCOPE_SIGNAL_BROWSER_H_
#define SCOPE_SIGNAL_BROWSER_H_

#include "scope/panel.h"

#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class QLineEdit;
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

// What can be plotted, as a topic -> field tree you can drag out of.
//
// Topics appear as soon as their publisher starts, whether or not it has ever
// published, and disappear-as-greyed when it stops. There is no rescan and no
// polling for traffic: every publisher declares a zenoh liveliness token, and
// the DataSource keeps the set current.
//
// Asking the source rather than the bus is deliberate. "Which topics exist" is
// a property of where the data comes from, so a recorded source will answer the
// same question from its file index and nothing here changes.
//
// NOTHING IS EVER EVICTED, only greyed. A row the user may have bound must not
// vanish underneath them, and unreachable is not the same as gone -- a network
// partition and a crash look identical from here.
class SignalBrowser : public QWidget
{
    Q_OBJECT

  public:
    explicit SignalBrowser(DataSource& source, QWidget* parent = nullptr);
    ~SignalBrowser() override;

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

    // Drains the directory into the tree. Called on a timer, and cheap when
    // nothing has changed because the directory's revision has not moved.
    void syncFromDirectory();
    void updateStatus();

    void addTopic(const QString& key, const QString& schema, bool reachable);
    void updateRowText(QTreeWidgetItem* topic);
    void onItemActivated(QTreeWidgetItem* item, int column);

    DataSource& source_;

    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;

    QTimer* directory_timer_ = nullptr;
    std::uint64_t last_directory_revision_ = 0;
};

}  // namespace scope

#endif  // SCOPE_SIGNAL_BROWSER_H_
