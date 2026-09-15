#ifndef SWITCHBOARD_SERVICE_LIST_H_
#define SWITCHBOARD_SERVICE_LIST_H_

#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QLineEdit;
class QTreeWidget;

namespace pub_sub
{
class NodeDirectory;
class ServiceDirectory;
}  // namespace pub_sub

namespace switchboard
{

// One advertised service, as the list shows it.
struct ServiceRow
{
    std::string key;
    std::string request_schema;
    std::string response_schema;
    std::string owner_zid;

    // The node's declared name, or empty when it has not declared one. The
    // list then shows a shortened zid instead -- see pub_sub::NodeDirectory.
    std::string owner_name;

    bool reachable = true;

    // How many REACHABLE nodes offer this key. More than one means a call
    // reaches all of them, which is worth a warning on the row.
    int offered_by = 1;
};

// Every advertised service, grouped by the node offering it.
//
// Fed by pub_sub::ServiceDirectory, which never forgets an entry -- a node that
// went away leaves its services behind marked unreachable. They collect under
// "offline (N)" at the bottom rather than disappearing, so a service you were
// just calling does not vanish from under the form when its node restarts.
class ServiceList : public QWidget
{
    Q_OBJECT

  public:
    explicit ServiceList(QWidget* parent = nullptr);
    ~ServiceList() override;

    // Re-reads the directories now instead of waiting for the next poll.
    void refresh();

    std::vector<ServiceRow> rows() const;

    // Selects a service, as a click would. With an empty owner the first
    // reachable offer of the key is taken. False when there is no such row.
    bool select(const std::string& key, const std::string& owner_zid = {});

    std::optional<ServiceRow> selected() const;

    // Shown in place of a name: the node's name, else a short zid.
    static std::string ownerLabel(const ServiceRow& row);

  signals:
    // The selection moved to a different service.
    void serviceSelected();

    // The directories changed -- including the selected service's own row,
    // e.g. its node going offline.
    void servicesChanged();

  private:
    void poll();
    void rebuild();

    std::unique_ptr<pub_sub::ServiceDirectory> services_;
    std::unique_ptr<pub_sub::NodeDirectory> nodes_;
    std::uint64_t service_revision_ = ~std::uint64_t{0};
    std::uint64_t node_revision_ = ~std::uint64_t{0};

    std::vector<ServiceRow> rows_;
    std::optional<ServiceRow> selected_;

    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTimer poll_timer_;
};

}  // namespace switchboard

#endif  // SWITCHBOARD_SERVICE_LIST_H_
