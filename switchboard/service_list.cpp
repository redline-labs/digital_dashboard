#include "switchboard/service_list.h"

#include "pub_sub/topic_directory.h"

#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace switchboard
{

namespace
{

constexpr int kKeyRole = Qt::UserRole + 1;
constexpr int kZidRole = Qt::UserRole + 2;

// Fast enough that a node starting feels immediate; the rebuild itself only
// happens when a directory's revision moves.
constexpr auto kPollInterval = std::chrono::milliseconds(250);

bool matches(const ServiceRow& row, const QString& filter)
{
    if (filter.isEmpty())
    {
        return true;
    }
    for (const std::string& text : {row.key, row.request_schema, row.response_schema,
                                    ServiceList::ownerLabel(row)})
    {
        if (QString::fromStdString(text).contains(filter, Qt::CaseInsensitive))
        {
            return true;
        }
    }
    return false;
}

}  // namespace

ServiceList::ServiceList(QWidget* parent) :
    QWidget(parent),
    services_(std::make_unique<pub_sub::ServiceDirectory>()),
    nodes_(std::make_unique<pub_sub::NodeDirectory>())
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel("Services");
    title->setStyleSheet("font-weight: 700; font-size: 13px;");
    layout->addWidget(title);

    filter_ = new QLineEdit();
    filter_->setObjectName("service_filter");
    filter_->setPlaceholderText("Filter by key, schema or node");
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    tree_ = new QTreeWidget();
    tree_->setObjectName("service_tree");
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_, 1);

    if (!services_->isValid())
    {
        auto* warning = new QLabel("Could not watch the bus for services: no zenoh session.");
        warning->setWordWrap(true);
        warning->setStyleSheet("color: #E74C3C;");
        layout->addWidget(warning);
    }

    connect(filter_, &QLineEdit::textChanged, this, [this]() { rebuild(); });

    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current)
            {
                if (current == nullptr || !current->data(0, kKeyRole).isValid())
                {
                    return;  // A group header, or the tree being rebuilt.
                }
                select(current->data(0, kKeyRole).toString().toStdString(),
                       current->data(0, kZidRole).toString().toStdString());
            });

    connect(&poll_timer_, &QTimer::timeout, this, [this]() { poll(); });
    poll_timer_.start(kPollInterval);

    refresh();
}

ServiceList::~ServiceList() = default;

void ServiceList::refresh()
{
    poll();
}

std::vector<ServiceRow> ServiceList::rows() const
{
    return rows_;
}

std::optional<ServiceRow> ServiceList::selected() const
{
    return selected_;
}

std::string ServiceList::ownerLabel(const ServiceRow& row)
{
    if (!row.owner_name.empty())
    {
        return row.owner_name;
    }
    if (!row.owner_zid.empty())
    {
        return "node " + row.owner_zid.substr(0, 8);
    }
    return "unknown node";
}

bool ServiceList::select(const std::string& key, const std::string& owner_zid)
{
    const ServiceRow* found = nullptr;
    for (const ServiceRow& row : rows_)
    {
        if (row.key != key || (!owner_zid.empty() && row.owner_zid != owner_zid))
        {
            continue;
        }
        // Prefer a reachable offer when the owner was not named.
        if (found == nullptr || (!found->reachable && row.reachable))
        {
            found = &row;
        }
    }
    if (found == nullptr)
    {
        return false;
    }

    const bool changed = !selected_ || selected_->key != found->key ||
                         selected_->owner_zid != found->owner_zid;
    selected_ = *found;

    // Reflect it in the tree without re-entering through currentItemChanged.
    {
        const QSignalBlocker blocker(tree_);
        for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it)
        {
            if ((*it)->data(0, kKeyRole).toString().toStdString() == selected_->key &&
                (*it)->data(0, kZidRole).toString().toStdString() == selected_->owner_zid)
            {
                tree_->setCurrentItem(*it);
                break;
            }
        }
    }

    if (changed)
    {
        emit serviceSelected();
    }
    return true;
}

void ServiceList::poll()
{
    const std::uint64_t services = services_->revision();
    const std::uint64_t nodes = nodes_->revision();
    if (services == service_revision_ && nodes == node_revision_)
    {
        return;
    }
    service_revision_ = services;
    node_revision_ = nodes;

    rows_.clear();
    std::map<std::string, int> reachable_offers;
    for (const pub_sub::ServiceEntry& entry : services_->snapshot())
    {
        ServiceRow row;
        row.key = entry.key;
        row.request_schema = entry.request_schema;
        row.response_schema = entry.response_schema;
        row.owner_zid = entry.owner_zid;
        row.owner_name = nodes_->nameFor(entry.owner_zid);
        row.reachable = entry.reachable;
        if (row.reachable)
        {
            ++reachable_offers[row.key];
        }
        rows_.push_back(std::move(row));
    }
    for (ServiceRow& row : rows_)
    {
        row.offered_by = std::max(1, reachable_offers[row.key]);
    }

    // Keep the selected row current -- reachability is exactly what changes.
    if (selected_)
    {
        for (const ServiceRow& row : rows_)
        {
            if (row.key == selected_->key && row.owner_zid == selected_->owner_zid)
            {
                selected_ = row;
                break;
            }
        }
    }

    rebuild();
    emit servicesChanged();
}

void ServiceList::rebuild()
{
    const QSignalBlocker blocker(tree_);
    tree_->clear();

    const QString filter = filter_->text().trimmed();

    std::map<std::string, QTreeWidgetItem*> groups;
    QTreeWidgetItem* offline = nullptr;
    int offline_count = 0;
    QTreeWidgetItem* current = nullptr;

    for (const ServiceRow& row : rows_)
    {
        if (!matches(row, filter))
        {
            continue;
        }

        QTreeWidgetItem* parent = nullptr;
        if (row.reachable)
        {
            const std::string label = ownerLabel(row);
            auto& group = groups[label];
            if (group == nullptr)
            {
                group = new QTreeWidgetItem(tree_);
                group->setText(0, QString::fromStdString(label));
                group->setToolTip(0, QString::fromStdString(row.owner_zid));
                QFont font = group->font(0);
                font.setBold(true);
                group->setFont(0, font);
                group->setFlags(Qt::ItemIsEnabled);
                group->setExpanded(true);
            }
            parent = group;
        }
        else
        {
            if (offline == nullptr)
            {
                offline = new QTreeWidgetItem();
                offline->setFlags(Qt::ItemIsEnabled);
            }
            parent = offline;
            ++offline_count;
        }

        auto* item = new QTreeWidgetItem(parent);
        QString text = QString::fromStdString((row.reachable ? "● " : "○ ") + row.key);
        QString tip = QString::fromStdString(row.request_schema + " → " + row.response_schema +
                                             "\n" + ownerLabel(row) + "  " + row.owner_zid);
        if (row.offered_by > 1)
        {
            text += QString("  ⚠ ×%1").arg(row.offered_by);
            tip += QString("\n\nOffered by %1 nodes: a call reaches all of them.").arg(row.offered_by);
        }
        if (!row.reachable)
        {
            item->setForeground(0, tree_->palette().color(QPalette::Disabled, QPalette::Text));
            tip += "\n\nIts node has gone away.";
        }
        item->setText(0, text);
        item->setToolTip(0, tip);
        item->setData(0, kKeyRole, QString::fromStdString(row.key));
        item->setData(0, kZidRole, QString::fromStdString(row.owner_zid));

        if (selected_ && row.key == selected_->key && row.owner_zid == selected_->owner_zid)
        {
            current = item;
        }
    }

    if (offline != nullptr)
    {
        offline->setText(0, QString("offline (%1)").arg(offline_count));
        tree_->addTopLevelItem(offline);
        offline->setExpanded(false);
    }

    if (current != nullptr)
    {
        tree_->setCurrentItem(current);
    }
}

}  // namespace switchboard
