#include "scope/signal_browser.h"

#include "scope/data_source.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"

#include <QApplication>
#include <QDrag>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>

#include <algorithm>

namespace scope
{

namespace
{

// Roles on the tree items. The candidate is reconstructed from these rather
// than kept in a parallel structure, so what is dragged is always exactly what
// is displayed.
constexpr int kRoleKey = Qt::UserRole + 1;
constexpr int kRoleSchema = Qt::UserRole + 2;
constexpr int kRoleField = Qt::UserRole + 3;
constexpr int kRoleCategory = Qt::UserRole + 4;

// Row state, kept on the item so updateRowText() can render it without
// consulting either source again.
constexpr int kRoleAdvertised = Qt::UserRole + 5;
constexpr int kRoleHz = Qt::UserRole + 6;  // < 0 means never observed.

BindingCandidate candidateOf(const QTreeWidgetItem* item)
{
    BindingCandidate candidate;
    candidate.zenoh_key = item->data(0, kRoleKey).toString().toStdString();
    candidate.schema_name = item->data(0, kRoleSchema).toString().toStdString();
    candidate.field_name = item->data(0, kRoleField).toString().toStdString();
    candidate.type_category = item->data(0, kRoleCategory).toString().toStdString();
    return candidate;
}

// A tree that drags candidates. Subclassed rather than configured because
// startDrag is the only hook for putting our own mime data on the drag.
class CandidateTree : public QTreeWidget
{
  public:
    using QTreeWidget::QTreeWidget;

  protected:
    void startDrag(Qt::DropActions supported) override
    {
        const QTreeWidgetItem* item = currentItem();
        if (item == nullptr || item->data(0, kRoleKey).toString().isEmpty())
        {
            return;
        }

        auto* mime = new QMimeData();
        mime->setData(kSignalMimeType, encodeCandidate(candidateOf(item)));
        // Plain text too, so dropping onto something outside the app gives
        // something readable rather than nothing.
        mime->setText(item->text(0));

        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(supported, Qt::CopyAction);
    }
};

}  // namespace

QByteArray encodeCandidate(const BindingCandidate& candidate)
{
    nlohmann::json payload;
    payload["zenoh_key"] = candidate.zenoh_key;
    payload["schema_name"] = candidate.schema_name;
    payload["field_name"] = candidate.field_name;
    payload["type_category"] = candidate.type_category;
    const std::string text = payload.dump();
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

bool decodeCandidate(const QByteArray& data, BindingCandidate& out)
{
    // Anything can be dropped on a widget, including a drag from a browser
    // window. Parsing has to fail cleanly rather than throw: an exception out
    // of a Qt event handler terminates the app, which is exactly how the
    // editor's canvas learned this (see Canvas::dragEnterEvent).
    const nlohmann::json payload =
        nlohmann::json::parse(data.begin(), data.end(), nullptr, /*allow_exceptions=*/false);
    if (payload.is_discarded() || !payload.is_object())
    {
        return false;
    }

    // Every field read through is_string() first, NOT through value(). value()
    // throws a type_error when the key is present but the wrong type -- so
    // {"zenoh_key": 42} would throw out of a Qt drop handler and terminate the
    // app, which is the exact failure this function exists to prevent and which
    // parse-with-allow_exceptions alone does not cover.
    const auto text = [&payload](const char* name) -> std::string {
        const auto found = payload.find(name);
        return found != payload.end() && found->is_string() ? found->get<std::string>()
                                                            : std::string{};
    };

    out.zenoh_key = text("zenoh_key");
    out.schema_name = text("schema_name");
    out.field_name = text("field_name");
    out.type_category = text("type_category");
    return !out.zenoh_key.empty();
}

SignalBrowser::SignalBrowser(DataSource& source, QWidget* parent) :
    QWidget(parent), source_(source)
{
    setObjectName("signal_browser");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    filter_ = new QLineEdit(this);
    filter_->setObjectName("browser_filter");
    filter_->setPlaceholderText(tr("Filter topics and fields"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    layout->addWidget(filter_);

    tree_ = new CandidateTree(this);
    tree_->setObjectName("browser_tree");
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Signal"), tr("Type")});
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(tree_, &QTreeWidget::itemActivated, this, &SignalBrowser::onItemActivated);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &SignalBrowser::onItemActivated);
    layout->addWidget(tree_, 1);

    rescan_button_ = new QPushButton(tr("Rescan"), this);
    rescan_button_->setObjectName("browser_rescan");
    connect(rescan_button_, &QPushButton::clicked, this, [this]() { rescan(); });
    layout->addWidget(rescan_button_);

    status_ = new QLabel(this);
    status_->setObjectName("browser_status");
    status_->setWordWrap(true);
    status_->setStyleSheet("color: palette(mid); font-size: 11px;");
    layout->addWidget(status_);

    // The advertisement watcher. Populated with history, so topics from nodes
    // that were already running when this window opened arrive immediately --
    // there is no initial query to forget and no window in between where a
    // node starting up could be missed.
    directory_ = std::make_unique<pub_sub::TopicDirectory>();
    if (!directory_->isValid())
    {
        SPDLOG_WARN("[browser] could not watch advertisements; falling back to observation only");
    }

    // Polled rather than pushed. The directory is updated from a zenoh RX
    // thread, which may not touch a QTreeWidget; draining it on a GUI timer is
    // the same discipline the sample path uses. It is cheap because the
    // revision counter makes an idle tick a single atomic load.
    directory_timer_ = new QTimer(this);
    directory_timer_->setObjectName("browser_directory_timer");
    connect(directory_timer_, &QTimer::timeout, this, &SignalBrowser::syncFromDirectory);
    directory_timer_->start(std::chrono::milliseconds(500));

    syncFromDirectory();
    updateStatus();
}

void SignalBrowser::syncFromDirectory()
{
    if (!directory_)
    {
        return;
    }

    const std::uint64_t revision = directory_->revision();
    if (revision == last_directory_revision_)
    {
        return;  // Nothing has come or gone; do not rebuild rows for nothing.
    }
    last_directory_revision_ = revision;

    for (const pub_sub::DirectoryEntry& entry : directory_->snapshot())
    {
        // -1 preserves whatever rate observation may already have recorded;
        // the directory knows nothing about traffic.
        addTopic(QString::fromStdString(entry.key), QString::fromStdString(entry.schema),
                 /*hz=*/-1.0, entry.reachable);
    }

    applyFilter();
    updateStatus();
}

// One line that is honest about both sources, because "no topics" would be a
// claim neither can support: the directory only knows who advertises, and
// observation only knows what published during a window.
void SignalBrowser::updateStatus()
{
    const int count = tree_->topLevelItemCount();

    if (count == 0)
    {
        status_->setText(ever_scanned_
                             ? tr("Nothing advertised, and nothing published during the scan. "
                                  "Topics appear here as soon as a node starts.")
                             : tr("Nothing advertised yet. Topics appear here as soon as a node "
                                  "starts -- no rescan needed. Rescan also picks up publishers "
                                  "that do not advertise, and measures rates."));
        return;
    }

    status_->setText(tr("%n topic(s). Drag a field onto a panel, or double-click it. "
                        "Rescan measures rates and finds publishers that do not advertise.",
                        nullptr, count));
}

SignalBrowser::~SignalBrowser() = default;

void SignalBrowser::rescan(int window_ms)
{
    const std::vector<TopicInfo> topics = source_.rescan(window_ms);
    ever_scanned_ = true;

    for (const TopicInfo& topic : topics)
    {
        addTopic(QString::fromStdString(topic.key), QString::fromStdString(topic.schema), topic.hz,
                 /*advertised=*/false);
    }

    // Rescan no longer decides the empty-state wording on its own -- the
    // directory may have populated the tree without any traffic at all.
    updateStatus();
    applyFilter();
}

void SignalBrowser::addTopic(const QString& key, const QString& schema, double hz,
                             bool advertised)
{
    // Merge, never replace. A topic is added by whichever source saw it first
    // and then updated in place, so an advertised topic that later carries
    // traffic keeps its row and simply gains a rate.
    QTreeWidgetItem* topic_item = nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
    {
        if (tree_->topLevelItem(i)->data(0, kRoleKey).toString() == key)
        {
            topic_item = tree_->topLevelItem(i);
            break;
        }
    }

    const bool is_new = topic_item == nullptr;
    if (is_new)
    {
        topic_item = new QTreeWidgetItem(tree_);
        topic_item->setData(0, kRoleKey, key);
        topic_item->setData(0, kRoleField, QString());
        topic_item->setData(0, kRoleCategory, QString());
        topic_item->setData(0, kRoleHz, -1.0);
    }

    topic_item->setText(0, key);
    topic_item->setData(0, kRoleSchema, schema);
    topic_item->setData(0, kRoleAdvertised, advertised);
    if (hz >= 0.0)
    {
        topic_item->setData(0, kRoleHz, hz);
    }
    updateRowText(topic_item);

    if (!is_new)
    {
        return;  // Fields already expanded; only the state needed refreshing.
    }

    // Fields come from the schema registry, not from a sample: the
    // advertisement (or the encoding) names the schema and the registry knows
    // its shape, so a topic's fields are known before any traffic at all.
    const auto capnp_schema = pub_sub::get_schema(schema.toStdString());
    if (!capnp_schema)
    {
        topic_item->setText(1, tr("unknown schema"));
        return;
    }

    const nlohmann::json described = pub_sub::describeSchema(*capnp_schema);
    const auto fields = described.find("fields");
    if (fields == described.end() || !fields->is_object())
    {
        return;
    }

    for (const auto& [name, info] : fields->items())
    {
        const std::string category = info.value("type", std::string{});

        auto* field_item = new QTreeWidgetItem(topic_item);
        field_item->setText(0, QString::fromStdString(name));
        field_item->setText(1, QString::fromStdString(category));
        field_item->setData(0, kRoleKey, key);
        field_item->setData(0, kRoleSchema, schema);
        field_item->setData(0, kRoleField, QString::fromStdString(name));
        field_item->setData(0, kRoleCategory, QString::fromStdString(category));

        // Non-numeric fields stay visible and draggable rather than being
        // hidden: a plot will decline them, but a future panel type may not,
        // and the browser should not have to learn about panel types to say so.
        BindingCandidate probe;
        probe.type_category = category;
        if (!probe.isNumeric())
        {
            field_item->setForeground(0, QColor("#808890"));
            field_item->setToolTip(0, tr("Not a numeric field; a plot cannot show it."));
        }
    }

    topic_item->setExpanded(true);
}

// Renders a topic row's second column and its colour from the two states.
//
// Greyed means "advertised but not reachable, or never seen" -- the row stays
// so a binding is never taken away, but it must not look live either. A list
// that silently claims a dead topic is producing is the actual failure mode.
void SignalBrowser::updateRowText(QTreeWidgetItem* topic)
{
    const bool advertised = topic->data(0, kRoleAdvertised).toBool();
    const double hz = topic->data(0, kRoleHz).toDouble();
    const QString schema = topic->data(0, kRoleSchema).toString();

    // Reachability decides first, and a measured rate does NOT override it.
    // Getting this backwards left a dead topic reading "27.5 Hz" in normal
    // colour after its publisher exited, because the last measurement was still
    // sitting on the row -- a stale number presented as a live one, which is
    // precisely the failure this column exists to prevent.
    QString state;
    if (advertised)
    {
        state = hz > 0.0 ? QStringLiteral("%1 Hz").arg(hz, 0, 'f', 1) : tr("advertised");
    }
    else if (hz > 0.0)
    {
        // Either the advertiser went away or it never advertised. Both mean the
        // rate is a historical measurement, so it is labelled as one.
        state = tr("%1 Hz (last seen)").arg(hz, 0, 'f', 1);
    }
    else
    {
        state = tr("unreachable");
    }

    topic->setText(1, state);
    topic->setForeground(0, advertised ? QBrush() : QBrush(QColor("#808890")));
    topic->setToolTip(0, advertised ? tr("Advertised by a live publisher.")
                                    : tr("Seen on the bus, but not advertised -- the publisher "
                                         "may have stopped, or may not advertise at all."));
}

void SignalBrowser::applyFilter()
{
    const QString needle = filter_->text().trimmed();

    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* topic = tree_->topLevelItem(i);

        // A match on the topic keeps all its fields, so filtering by topic name
        // is a way to see everything one node publishes.
        const bool topic_matches =
            needle.isEmpty() || topic->text(0).contains(needle, Qt::CaseInsensitive);

        int visible_children = 0;
        for (int j = 0; j < topic->childCount(); ++j)
        {
            QTreeWidgetItem* field = topic->child(j);
            const bool field_matches =
                topic_matches || field->text(0).contains(needle, Qt::CaseInsensitive);
            field->setHidden(!field_matches);
            visible_children += field_matches ? 1 : 0;
        }

        topic->setHidden(!topic_matches && visible_children == 0);
        if (!needle.isEmpty() && visible_children > 0)
        {
            topic->setExpanded(true);
        }
    }
}

void SignalBrowser::onItemActivated(QTreeWidgetItem* item, int /*column*/)
{
    if (item == nullptr || item->data(0, kRoleField).toString().isEmpty())
    {
        return;  // A topic row; nothing to plot on its own.
    }
    emit candidateActivated(candidateOf(item));
}

std::vector<BindingCandidate> SignalBrowser::candidates() const
{
    std::vector<BindingCandidate> all;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
    {
        const QTreeWidgetItem* topic = tree_->topLevelItem(i);
        all.push_back(candidateOf(topic));
        for (int j = 0; j < topic->childCount(); ++j)
        {
            all.push_back(candidateOf(topic->child(j)));
        }
    }
    return all;
}

bool SignalBrowser::findCandidate(const QString& zenoh_key,
                                  const QString& field_name,
                                  BindingCandidate& out) const
{
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
    {
        const QTreeWidgetItem* topic = tree_->topLevelItem(i);
        if (topic->data(0, kRoleKey).toString() != zenoh_key)
        {
            continue;
        }

        if (field_name.isEmpty())
        {
            out = candidateOf(topic);
            return true;
        }

        for (int j = 0; j < topic->childCount(); ++j)
        {
            const QTreeWidgetItem* field = topic->child(j);
            if (field->data(0, kRoleField).toString() == field_name)
            {
                out = candidateOf(field);
                return true;
            }
        }
        return false;
    }
    return false;
}

}  // namespace scope

#include "scope/moc_signal_browser.cpp"
