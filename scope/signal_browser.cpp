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

// Reachability, kept on the item so updateRowText() can render without asking
// the source again.
constexpr int kRoleAdvertised = Qt::UserRole + 5;

// A list's element category. Separate from kRoleCategory because "list" alone
// does not say whether anything can be done with it.
constexpr int kRoleElementCategory = Qt::UserRole + 6;

// Which element of a list a row names, or -1 for the list itself.
constexpr int kRoleElementIndex = Qt::UserRole + 7;

BindingCandidate candidateOf(const QTreeWidgetItem* item)
{
    BindingCandidate candidate;
    candidate.zenoh_key = item->data(0, kRoleKey).toString().toStdString();
    candidate.schema_name = item->data(0, kRoleSchema).toString().toStdString();
    candidate.field_name = item->data(0, kRoleField).toString().toStdString();
    candidate.type_category = item->data(0, kRoleCategory).toString().toStdString();
    candidate.element_category =
        item->data(0, kRoleElementCategory).toString().toStdString();
    {
        const QVariant index = item->data(0, kRoleElementIndex);
        candidate.element_index = index.isValid() ? index.toInt() : -1;
    }
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
    payload["element_category"] = candidate.element_category;
    payload["element_index"] = candidate.element_index;
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
    out.element_category = text("element_category");
    {
        // Read through is_number_integer() for the same reason every string
        // above goes through is_string(): value() throws on a present-but-wrong
        // type, and that exception would come out of a Qt drop handler.
        const auto found = payload.find("element_index");
        out.element_index =
            found != payload.end() && found->is_number_integer() ? found->get<int>() : -1;
    }
    return !out.zenoh_key.empty();
}

SignalBrowser::SignalBrowser(DataSource& source, QWidget* parent) :
    QWidget(parent), source_(&source)
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

    status_ = new QLabel(this);
    status_->setObjectName("browser_status");
    status_->setWordWrap(true);
    status_->setStyleSheet("color: palette(mid); font-size: 11px;");
    layout->addWidget(status_);

    // Polled rather than pushed. The source learns about topics on a zenoh RX
    // thread, which may not touch a QTreeWidget; draining on a GUI timer is the
    // same discipline the sample path uses. An idle tick costs one atomic load,
    // because the revision is compared before anything is rebuilt.
    directory_timer_ = new QTimer(this);
    directory_timer_->setObjectName("browser_directory_timer");
    connect(directory_timer_, &QTimer::timeout, this, &SignalBrowser::syncFromDirectory);
    directory_timer_->start(std::chrono::milliseconds(500));

    syncFromDirectory();
    updateStatus();
}

void SignalBrowser::setSource(DataSource& source)
{
    if (&source == source_)
    {
        return;
    }
    source_ = &source;

    // Cleared, not merged. Every row here described what the OLD source
    // offered, and the usual "never evict, only grey" rule does not apply
    // across a swap: a topic missing from a recording is not unreachable and
    // will not come back, so leaving it greyed would invite someone to bind it
    // and wait for data that cannot arrive.
    tree_->clear();

    // Zero rather than the new source's revision, so the first sync always
    // rebuilds. A fresh source starting at revision 0 would otherwise match
    // whatever the old one happened to be on and the tree would stay empty.
    last_directory_revision_ = 0;

    syncFromDirectory();
    updateStatus();
}

void SignalBrowser::syncFromDirectory()
{
    const std::uint64_t revision = source_->topicsRevision();
    if (revision == last_directory_revision_)
    {
        return;  // Nothing has come or gone; do not rebuild rows for nothing.
    }
    last_directory_revision_ = revision;

    for (const TopicInfo& topic : source_->topics())
    {
        addTopic(QString::fromStdString(topic.key), QString::fromStdString(topic.schema),
                 topic.reachable);
    }

    applyFilter();
    updateStatus();
}

// The empty state is a statement about advertisements, not about the bus.
void SignalBrowser::updateStatus()
{
    const int count = tree_->topLevelItemCount();
    status_->setText(count == 0
                         ? tr("No topics yet. They appear here as soon as a publisher starts -- "
                              "nothing needs to be published first.")
                         : tr("%n topic(s). Drag a field onto a panel, or double-click it.",
                              nullptr, count));
}

SignalBrowser::~SignalBrowser() = default;

void SignalBrowser::addTopic(const QString& key, const QString& schema, bool reachable)
{
    // Merge, never replace. A topic already listed is updated in place, so a
    // publisher restarting refreshes its row rather than duplicating it.
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
    }

    topic_item->setText(0, key);
    topic_item->setData(0, kRoleSchema, schema);
    topic_item->setData(0, kRoleAdvertised, reachable);
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
        field_item->setData(0, kRoleElementCategory,
                            QString::fromStdString(info.value("element_type", std::string{})));

        field_item->setData(0, kRoleElementIndex, -1);

        // A LIST EXPANDS INTO ONE ROW PER ELEMENT, but only when the schema says
        // how many there are.
        //
        // capnp declares no length -- the count is a property of each message --
        // so the alternatives were to guess one (offering rows that do not
        // exist, which bind and then produce nothing, looking exactly like a
        // dead publisher) or to peek at live traffic (a browser that shows
        // different things depending on whether anything happened to be
        // flowing). A $fixedLength annotation makes it a fact about the schema,
        // which is where it belongs. See schemas/annotations.capnp.
        const std::string element_category = info.value("element_type", std::string{});
        if (category == "list")
        {
            const auto length = info.find("fixed_length");
            if (length != info.end() && length->is_number_unsigned())
            {
                const unsigned count = length->get<unsigned>();
                field_item->setText(1, QString("list[%1] of %2")
                                           .arg(count)
                                           .arg(QString::fromStdString(element_category)));

                for (unsigned i = 0; i < count; ++i)
                {
                    auto* element = new QTreeWidgetItem(field_item);
                    element->setText(0, QString("%1[%2]")
                                            .arg(QString::fromStdString(name))
                                            .arg(i));
                    element->setText(1, QString::fromStdString(element_category));
                    element->setData(0, kRoleKey, key);
                    element->setData(0, kRoleSchema, schema);
                    element->setData(0, kRoleField, QString::fromStdString(name));
                    element->setData(0, kRoleCategory, QString::fromStdString(category));
                    element->setData(0, kRoleElementCategory,
                                     QString::fromStdString(element_category));
                    element->setData(0, kRoleElementIndex, static_cast<int>(i));

                    BindingCandidate element_probe;
                    element_probe.type_category = category;
                    element_probe.element_category = element_category;
                    if (!element_probe.isNumeric())
                    {
                        element->setForeground(0, QColor("#808890"));
                    }
                }

                // Collapsed. Thirty-two rows per list, several lists per PDM
                // topic: expanded by default they would bury every other topic
                // in the tree.
                field_item->setExpanded(false);
            }
            else
            {
                // Honest about not knowing, rather than silently offering no
                // elements as though the list were empty.
                field_item->setText(1, QString("list of %1 (length not declared)")
                                           .arg(QString::fromStdString(element_category)));
            }
        }

        // Non-numeric fields stay visible and draggable rather than being
        // hidden: a plot will decline them, but a future panel type may not,
        // and the browser should not have to learn about panel types to say so.
        BindingCandidate probe;
        probe.type_category = category;
        probe.element_category = element_category;
        if (!probe.isNumeric())
        {
            field_item->setForeground(0, QColor("#808890"));
            field_item->setToolTip(0, tr("Not a numeric field; a plot cannot show it."));
        }
    }

    topic_item->setExpanded(true);
}

// Renders a topic row's state.
//
// Greyed means the publisher is no longer reachable. The row STAYS -- a binding
// the user may already have made must not vanish underneath them, and a
// liveliness DELETE means "unreachable from here" rather than "gone", since a
// network partition and a crash are indistinguishable.
void SignalBrowser::updateRowText(QTreeWidgetItem* topic)
{
    const bool reachable = topic->data(0, kRoleAdvertised).toBool();
    const QString schema = topic->data(0, kRoleSchema).toString();

    topic->setText(1, reachable ? schema : tr("unreachable"));
    topic->setForeground(0, reachable ? QBrush() : QBrush(QColor("#808890")));
    topic->setToolTip(0, reachable
                             ? tr("Advertised by a live publisher.")
                             : tr("The publisher is no longer reachable. The topic stays listed "
                                  "so an existing binding is not lost."));
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
            const QTreeWidgetItem* field = topic->child(j);
            all.push_back(candidateOf(field));

            // AND THE ELEMENT ROWS UNDER A LIST. This used to stop at the
            // field level, which was right while the tree was two deep. A list
            // whose length the schema declares expands into a row per element,
            // and those rows are the whole point -- missing them here would
            // leave them draggable in the UI but invisible to `scope.browser`
            // and unreachable by `scope.add_signal`, so the feature would work
            // only for a human with a mouse.
            for (int k = 0; k < field->childCount(); ++k)
            {
                all.push_back(candidateOf(field->child(k)));
            }
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
