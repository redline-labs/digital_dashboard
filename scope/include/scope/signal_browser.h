#ifndef SCOPE_SIGNAL_BROWSER_H_
#define SCOPE_SIGNAL_BROWSER_H_

#include "scope/panel.h"

#include "pub_sub/topic_directory.h"

#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
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
// TWO SOURCES, UNIONED, and the difference matters.
//
// The primary one is pub_sub::TopicDirectory: every publisher declares a
// liveliness token when it is constructed, so a topic appears here the moment
// its node starts, whether or not it has ever published. No rescan, no polling,
// no waiting for traffic. That is what makes topics "just appear".
//
// The secondary one is pub_sub::observeTopics, which listens for a window and
// reports what arrived. It cannot see a topic that has said nothing -- a silent
// topic is indistinguishable from an absent one -- so it can never replace the
// directory. It is kept for two things the directory cannot give: publishers
// that bypass BytePublisher and so advertise nothing, and the live sample rate.
//
// Rows are therefore in one of three states, and the tree says which:
//
//   advertised + seen   a node is up and data is flowing
//   advertised          a node is up but has published nothing yet, or has
//                       stopped -- the unplugged-CAN-adapter case
//   seen                traffic from something that does not advertise
//
// NOTHING IS EVER EVICTED, only greyed. A row the user may have bound must not
// vanish underneath them, and a liveliness DELETE means "unreachable from here"
// rather than "gone" -- a network partition and a crash are indistinguishable,
// so removing the row would claim more than zenoh actually told us.
class SignalBrowser : public QWidget
{
    Q_OBJECT

  public:
    explicit SignalBrowser(DataSource& source, QWidget* parent = nullptr);
    ~SignalBrowser() override;

    // Subscribe for `window_ms` and merge observed traffic into the tree.
    // Blocks for that long, so it is called from a button, never from a paint.
    // Only needed for rates and for publishers that do not advertise; the
    // directory populates the tree on its own.
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

    // Drains the directory into the tree. Called on a timer, and cheap when
    // nothing has changed because the directory's revision has not moved.
    void syncFromDirectory();
    void updateStatus();

    // `hz < 0` means "not observed", which is how an advertised-but-silent
    // topic is added.
    void addTopic(const QString& key, const QString& schema, double hz, bool advertised);
    void updateRowText(QTreeWidgetItem* topic);
    void onItemActivated(QTreeWidgetItem* item, int column);

    DataSource& source_;

    QLineEdit* filter_ = nullptr;
    QPushButton* rescan_button_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;

    // Watches the advertisement space for this browser's lifetime. Declared
    // before the tree it writes into so it is destroyed after it.
    std::unique_ptr<pub_sub::TopicDirectory> directory_;
    QTimer* directory_timer_ = nullptr;
    std::uint64_t last_directory_revision_ = 0;

    bool ever_scanned_ = false;
};

}  // namespace scope

#endif  // SCOPE_SIGNAL_BROWSER_H_
