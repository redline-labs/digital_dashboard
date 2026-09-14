#ifndef SWITCHBOARD_SWITCHBOARD_WINDOW_H_
#define SWITCHBOARD_SWITCHBOARD_WINDOW_H_

#include "switchboard/call_history.h"
#include "switchboard/response_view.h"
#include "switchboard/schema_form.h"
#include "switchboard/service_list.h"

#include "pub_sub/dynamic_service_call.h"

#include <QMainWindow>

#include <memory>
#include <optional>
#include <string>

class QLabel;
class QPushButton;
class QSpinBox;

namespace switchboard
{

// Services on the left, the selected one's request form in the middle, and what
// came back -- plus every call made to it this session -- on the right.
class SwitchboardWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit SwitchboardWindow(int default_timeout_ms = 2000, QWidget* parent = nullptr);
    ~SwitchboardWindow() override;

    ServiceList& serviceList() { return *services_; }
    SchemaForm& form() { return *form_; }
    ResponseView& response() { return *response_; }
    CallHistory& history() { return *history_; }

    // As a click in the list would. False, with `error` set, when there is no
    // such service.
    bool selectService(const std::string& key, const std::string& owner_zid, std::string& error);

    std::optional<ServiceRow> currentService() const { return current_; }

    // Sends the form. Returns the call id, or -1 with `error` set when nothing
    // could be sent: no service, an invalid form, or a call still in flight.
    // The result arrives later -- through history(), and in the response pane
    // if this service is still the one showing.
    int submit(std::optional<int> timeout_ms, std::string& error);

    bool callInFlight() const { return inflight_call_id_ != 0; }
    int timeoutMs() const;

  private:
    // Shared with every in-flight call's completion closure, which runs on a
    // zenoh thread and may outlive the window. The window nulls the pointer
    // under the lock as it is destroyed; a closure posts to the window only
    // while holding the same lock -- so it either reaches a live window or
    // does nothing. See the destructor.
    struct Liveness;

    void onServiceSelected();
    void onServicesChanged();
    void onCallFinished(int call_id, pub_sub::ServiceCallResult result);
    void onHistoryActivated(int call_id);
    void showServiceHeader();
    void updateSubmitState();

    std::shared_ptr<Liveness> liveness_;

    ServiceList* services_ = nullptr;
    SchemaForm* form_ = nullptr;
    ResponseView* response_ = nullptr;
    CallHistory* history_ = nullptr;

    QLabel* title_ = nullptr;
    QLabel* schemas_ = nullptr;
    QLabel* doc_ = nullptr;
    QLabel* warning_ = nullptr;
    QLabel* form_problems_ = nullptr;
    QSpinBox* timeout_ = nullptr;
    QPushButton* reset_ = nullptr;
    QPushButton* submit_ = nullptr;

    std::optional<ServiceRow> current_;
    int next_call_id_ = 1;
    int inflight_call_id_ = 0;
};

}  // namespace switchboard

#endif  // SWITCHBOARD_SWITCHBOARD_WINDOW_H_
