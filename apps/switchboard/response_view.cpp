#include "switchboard/response_view.h"

#include "switchboard/schema_form.h"

#include "pub_sub/schema_registry.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace switchboard
{

namespace
{

constexpr int kJsonRole = Qt::UserRole + 1;

// Lists longer than this start collapsed: a map reply with 400 route points
// should show "points [400]", not push every other field off the screen.
constexpr std::size_t kExpandListsUpTo = 20;

constexpr const char* kErrorBanner =
    "background: #2B1A18; border: 1px solid #C0392B; color: #E74C3C; padding: 6px; "
    "border-radius: 4px;";

// The type of member `name` of `type`, when `type` is a struct that has one.
std::optional<capnp::Type> memberType(const std::optional<capnp::Type>& type, const std::string& name)
{
    if (!type || !type->isStruct())
    {
        return std::nullopt;
    }
    KJ_IF_MAYBE(field, type->asStruct().findFieldByName(name))
    {
        return field->getType();
    }
    return std::nullopt;
}

std::optional<capnp::Type> elementType(const std::optional<capnp::Type>& type)
{
    if (!type || !type->isList())
    {
        return std::nullopt;
    }
    return type->asList().getElementType();
}

QString scalarText(const json& value)
{
    if (value.is_null())
    {
        return "—";
    }
    if (value.is_string())
    {
        return QString::fromStdString(value.get<std::string>());
    }
    return QString::fromStdString(value.dump());
}

void addValue(QTreeWidgetItem* parent, const QString& name, const json& value,
              const std::optional<capnp::Type>& type)
{
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, name);
    item->setText(2, type ? QString::fromStdString(capnpTypeName(*type)) : QString());
    item->setData(0, kJsonRole, QString::fromStdString(value.dump(2)));

    const bool is_data = type && type->isData();

    if (is_data && value.is_object())
    {
        // Data too long to inline: its length, and the start of it.
        const auto bytes = value.value("_data_bytes", std::size_t{0});
        const std::string prefix = value.value("hex_prefix", std::string());
        item->setText(1, QString("%1 bytes: %2…").arg(bytes).arg(QString::fromStdString(prefix)));
    }
    else if (value.is_object())
    {
        item->setText(1, type && type->isStruct()
                             ? QString::fromStdString(capnpTypeName(*type))
                             : QString("{%1}").arg(value.size()));
        for (const auto& [key, child] : value.items())
        {
            addValue(item, QString::fromStdString(key), child, memberType(type, key));
        }
        item->setExpanded(true);
    }
    else if (value.is_array())
    {
        item->setText(1, QString("[%1]").arg(value.size()));
        const auto element = elementType(type);
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            addValue(item, QString("[%1]").arg(i), value[i], element);
        }
        item->setExpanded(value.size() <= kExpandListsUpTo);
    }
    else if (is_data && value.is_string())
    {
        // Grouped in bytes, which is how anyone reads a command frame.
        const std::string hex = value.get<std::string>();
        QString grouped;
        for (std::size_t i = 0; i < hex.size(); i += 2)
        {
            if (!grouped.isEmpty())
            {
                grouped += ' ';
            }
            grouped += QString::fromStdString(hex.substr(i, 2));
        }
        item->setText(1, grouped.isEmpty() ? QString("(empty)") : grouped);
    }
    else
    {
        item->setText(1, scalarText(value));
        if (value.is_boolean() && name == "ok")
        {
            item->setForeground(1, value.get<bool>() ? QColor("#2ECC71") : QColor("#E74C3C"));
        }
    }
}

QTreeWidget* makeTree(const pub_sub::ServiceReply& reply, int index)
{
    auto* tree = new QTreeWidget();
    tree->setObjectName(QString("response_tree_%1").arg(index));
    tree->setColumnCount(3);
    tree->setHeaderLabels({"Field", "Value", "Type"});
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    tree->setAlternatingRowColors(true);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);

    std::optional<capnp::Type> root_type;
    if (const auto schema = pub_sub::get_schema(reply.schema))
    {
        root_type = capnp::Type(schema->asStruct());
    }

    if (reply.value.is_object())
    {
        for (const auto& [key, child] : reply.value.items())
        {
            addValue(tree->invisibleRootItem(), QString::fromStdString(key), child,
                     memberType(root_type, key));
        }
    }

    QObject::connect(tree, &QTreeWidget::customContextMenuRequested, tree,
                     [tree](const QPoint& pos)
                     {
                         QTreeWidgetItem* item = tree->itemAt(pos);
                         if (item == nullptr)
                         {
                             return;
                         }
                         QMenu menu(tree);
                         menu.addAction("Copy value", [item]()
                                        { QApplication::clipboard()->setText(item->text(1)); });
                         menu.addAction("Copy as JSON",
                                        [item]() {
                                            QApplication::clipboard()->setText(
                                                item->data(0, kJsonRole).toString());
                                        });
                         menu.exec(tree->viewport()->mapToGlobal(pos));
                     });

    // Tall enough for what it holds, up to a point; the right pane scrolls.
    tree->setMinimumHeight(120);
    return tree;
}

}  // namespace

ResponseView::ResponseView(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel("Response");
    title->setStyleSheet("font-weight: 700; font-size: 13px;");
    layout->addWidget(title);

    status_ = new QLabel();
    status_->setObjectName("response_status");
    status_->setStyleSheet("font-weight: 600;");
    layout->addWidget(status_);

    note_ = new QLabel();
    note_->setObjectName("response_note");
    note_->setStyleSheet("color: palette(mid); font-size: 11px;");
    note_->setWordWrap(true);
    layout->addWidget(note_);

    banner_ = new QLabel();
    banner_->setObjectName("response_banner");
    banner_->setWordWrap(true);
    banner_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    banner_->setStyleSheet(kErrorBanner);
    layout->addWidget(banner_);

    sections_ = new QWidget();
    sections_layout_ = new QVBoxLayout(sections_);
    sections_layout_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(sections_, 1);

    clear("Select a service and submit a request.");
}

void ResponseView::clear(const QString& message)
{
    status_->setText(message);
    note_->clear();
    note_->hide();
    banner_->clear();
    banner_->hide();

    while (QLayoutItem* child = sections_layout_->takeAt(0))
    {
        if (QWidget* widget = child->widget())
        {
            // Detached before the deferred delete, so until the event loop gets
            // to it the old tree cannot be found by name -- by ui_find, or by a
            // test -- in place of the new one.
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete child;
    }
    section_count_ = 0;
}

void ResponseView::showPending(const QString& key)
{
    clear(QString("… Calling %1").arg(key));
}

void ResponseView::showResult(const pub_sub::ServiceCallResult& result, const QString& note)
{
    using Status = pub_sub::ServiceCallResult::Status;
    clear();

    QStringList banner;
    QString status;
    const QString elapsed = QString("%1 ms").arg(result.elapsed.count());

    switch (result.status)
    {
        case Status::Replied:
        {
            const auto errors = std::count_if(result.replies.begin(), result.replies.end(),
                                              [](const auto& reply) { return reply.is_error; });
            if (errors == static_cast<long>(result.replies.size()))
            {
                status = "⚠ Service error · " + elapsed;
            }
            else if (result.replies.size() > 1)
            {
                status = QString("✔ %1 replies · %2").arg(result.replies.size()).arg(elapsed);
            }
            else
            {
                status = "✔ Replied · " + elapsed;
            }
            break;
        }
        case Status::NoReply:
            // `elapsed` is the timeout here -- zenoh ended the query at it.
            status = QString("✖ No reply after %1").arg(elapsed);
            banner << "Nothing answered. Either no node serves this key any more, or it did "
                      "not reply within the timeout -- zenoh cannot tell those apart.";
            break;
        case Status::RequestRejected:
            status = "✖ Request rejected — nothing was sent";
            for (const std::string& error : result.errors)
            {
                banner << QString::fromStdString(error);
            }
            break;
        case Status::Failed:
            status = "✖ Call failed";
            for (const std::string& error : result.errors)
            {
                banner << QString::fromStdString(error);
            }
            break;
    }

    // Two replies to one call is almost always two nodes offering one key. Say
    // so, and say what cannot be known: zenoh's replier id is an unstable API
    // this build does not enable, so a reply cannot be tied to its node.
    QStringList notes;
    if (!note.isEmpty())
    {
        notes << note;
    }
    if (result.replies.size() > 1)
    {
        notes << QString("%1 nodes answered; replies cannot be attributed to a node.")
                     .arg(result.replies.size());
    }

    int index = 0;
    for (const pub_sub::ServiceReply& reply : result.replies)
    {
        if (result.replies.size() > 1)
        {
            auto* header = new QLabel(QString("Reply %1 of %2").arg(index + 1).arg(result.replies.size()));
            header->setStyleSheet("font-weight: 600;");
            sections_layout_->addWidget(header);
        }

        if (reply.is_error)
        {
            banner << "Service error: " + QString::fromStdString(reply.error_text);
            auto* error = new QLabel(QString::fromStdString(reply.error_text));
            error->setWordWrap(true);
            error->setStyleSheet("color: #E74C3C;");
            sections_layout_->addWidget(error);
        }
        else
        {
            // The two ways a service in this tree says no:
            //   ok :Bool + error/message :Text      -- can_bridge, grayhill, bd992
            //   status :SomeEnum + error :Text      -- every map service, whose
            //                                          enums spell success `ok`
            // Either signal is enough on its own. map/route with a missing graph
            // answers status = noSuchGraph beside a zero-metre route, and a
            // zero-metre route under a green tick reads as a success.
            if (reply.value.is_object())
            {
                const json& value = reply.value;
                std::string reason;
                for (const char* field : {"error", "message"})
                {
                    if (value.contains(field) && value[field].is_string() &&
                        !value[field].get<std::string>().empty())
                    {
                        reason = value[field].get<std::string>();
                        break;
                    }
                }
                const std::string reply_status =
                    (value.contains("status") && value["status"].is_string())
                        ? value["status"].get<std::string>()
                        : std::string();

                const bool has_ok = value.contains("ok") && value["ok"].is_boolean();
                if (has_ok && !value["ok"].get<bool>())
                {
                    banner << (reason.empty() ? QString("The service answered ok = false.")
                                              : "ok = false: " + QString::fromStdString(reason));
                }
                else if (!has_ok &&
                         (!reason.empty() || (!reply_status.empty() && reply_status != "ok")))
                {
                    QString line = reply_status.empty() || reply_status == "ok"
                                       ? QString("The service reported an error")
                                       : QString("status = %1").arg(QString::fromStdString(reply_status));
                    if (!reason.empty())
                    {
                        line += ": " + QString::fromStdString(reason);
                    }
                    banner << line;
                }
            }
            sections_layout_->addWidget(makeTree(reply, index), 1);
        }
        ++index;
    }
    section_count_ = index;

    status_->setText(status);
    if (!notes.isEmpty())
    {
        note_->setText(notes.join("\n"));
        note_->show();
    }
    if (!banner.isEmpty())
    {
        banner_->setText(banner.join("\n"));
        banner_->show();
    }
}

QString ResponseView::statusText() const
{
    return status_->text();
}

QString ResponseView::bannerText() const
{
    return banner_->isHidden() ? QString() : banner_->text();
}

int ResponseView::replySectionCount() const
{
    return section_count_;
}

}  // namespace switchboard
