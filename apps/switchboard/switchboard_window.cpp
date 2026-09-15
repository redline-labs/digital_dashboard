#include "switchboard/switchboard_window.h"

#include "pub_sub/schema_registry.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include <mutex>

namespace switchboard
{

namespace
{

// A reply worth reading as fields is small; Data past this shows its length
// and a prefix instead of a wall of hex. See pub_sub::CapnpJsonOptions.
constexpr std::size_t kReplyHexLimit = 4096;

// Enough to show what is wrong without the list pushing the Submit button off
// the bottom of the window.
constexpr std::size_t kProblemsShown = 5;

QWidget* padded(QWidget* inner)
{
    auto* outer = new QWidget();
    auto* layout = new QVBoxLayout(outer);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(inner);
    return outer;
}

QLabel* secondaryLabel(const char* object_name)
{
    auto* label = new QLabel();
    label->setObjectName(object_name);
    label->setStyleSheet("color: palette(mid); font-size: 11px;");
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

}  // namespace

struct SwitchboardWindow::Liveness
{
    std::mutex mutex;
    SwitchboardWindow* window = nullptr;
};

SwitchboardWindow::SwitchboardWindow(int default_timeout_ms, QWidget* parent) :
    QMainWindow(parent),
    liveness_(std::make_shared<Liveness>())
{
    liveness_->window = this;

    setWindowTitle("switchboard");
    resize(1320, 780);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setObjectName("main_splitter");
    setCentralWidget(splitter);

    // ---- left: services
    services_ = new ServiceList();
    splitter->addWidget(padded(services_));

    // ---- middle: the selected service and its request
    auto* center = new QWidget();
    auto* center_layout = new QVBoxLayout(center);
    center_layout->setContentsMargins(0, 0, 0, 0);

    title_ = new QLabel();
    title_->setObjectName("service_title");
    title_->setStyleSheet("font-weight: 700; font-size: 15px;");
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    center_layout->addWidget(title_);

    schemas_ = secondaryLabel("service_schemas");
    center_layout->addWidget(schemas_);

    doc_ = new QLabel();
    doc_->setObjectName("service_doc");
    doc_->setWordWrap(true);
    doc_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    center_layout->addWidget(doc_);

    warning_ = new QLabel();
    warning_->setObjectName("service_warning");
    warning_->setWordWrap(true);
    warning_->setStyleSheet("color: #E67E22;");
    center_layout->addWidget(warning_);

    form_ = new SchemaForm();
    auto* form_holder = new QWidget();
    auto* form_holder_layout = new QVBoxLayout(form_holder);
    form_holder_layout->setContentsMargins(0, 4, 8, 4);
    form_holder_layout->addWidget(form_);
    form_holder_layout->addStretch(1);

    auto* scroll = new QScrollArea();
    scroll->setObjectName("form_scroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(form_holder);
    center_layout->addWidget(scroll, 1);

    form_problems_ = new QLabel();
    form_problems_->setObjectName("form_problems");
    form_problems_->setWordWrap(true);
    form_problems_->setStyleSheet("color: #E74C3C; font-size: 11px;");
    center_layout->addWidget(form_problems_);

    auto* controls = new QHBoxLayout();
    controls->addWidget(new QLabel("Timeout"));
    timeout_ = new QSpinBox();
    timeout_->setObjectName("timeout_ms");
    timeout_->setRange(50, 600000);
    timeout_->setSingleStep(100);
    timeout_->setSuffix(" ms");
    timeout_->setValue(default_timeout_ms);
    controls->addWidget(timeout_);
    controls->addStretch(1);

    reset_ = new QPushButton("Reset");
    reset_->setObjectName("reset_button");
    reset_->setToolTip("Put every field back to the schema's defaults.");
    controls->addWidget(reset_);

    submit_ = new QPushButton("Submit");
    submit_->setObjectName("submit_button");
    submit_->setDefault(true);
    submit_->setToolTip(QString("Send the request (%1+Enter).")
                            .arg(QKeySequence(Qt::CTRL).toString(QKeySequence::NativeText)));
    controls->addWidget(submit_);
    center_layout->addLayout(controls);

    splitter->addWidget(padded(center));

    // ---- right: what came back, and every call this session
    auto* right = new QSplitter(Qt::Vertical);
    response_ = new ResponseView();
    history_ = new CallHistory();
    right->addWidget(padded(response_));
    right->addWidget(padded(history_));
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 1);
    splitter->addWidget(right);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 4);
    splitter->setStretchFactor(2, 4);
    splitter->setSizes({300, 520, 500});

    // ---- wiring
    connect(services_, &ServiceList::serviceSelected, this, [this]() { onServiceSelected(); });
    connect(services_, &ServiceList::servicesChanged, this, [this]() { onServicesChanged(); });
    connect(form_, &SchemaForm::edited, this, [this]() { updateSubmitState(); });
    connect(form_, &SchemaForm::validityChanged, this, [this]() { updateSubmitState(); });
    connect(reset_, &QPushButton::clicked, form_, &SchemaForm::resetToDefaults);
    connect(history_, &CallHistory::recordActivated, this,
            [this](int call_id) { onHistoryActivated(call_id); });

    const auto submitFromUi = [this]()
    {
        std::string error;
        if (submit(std::nullopt, error) < 0 && !error.empty())
        {
            // Normally unreachable -- the button is disabled in every case
            // submit() refuses -- but the shortcut is not a button.
            response_->clear(QString::fromStdString("✖ " + error));
        }
    };
    connect(submit_, &QPushButton::clicked, this, submitFromUi);
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter})
    {
        auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | key), this);
        connect(shortcut, &QShortcut::activated, this, submitFromUi);
    }

    showServiceHeader();
    updateSubmitState();
}

SwitchboardWindow::~SwitchboardWindow()
{
    // After this, a call completing on a zenoh thread finds no window and posts
    // nothing. Taking the lock waits out a closure that is mid-post; one that
    // already posted leaves an event Qt discards along with this object.
    const std::lock_guard<std::mutex> guard(liveness_->mutex);
    liveness_->window = nullptr;
}

int SwitchboardWindow::timeoutMs() const
{
    return timeout_->value();
}

bool SwitchboardWindow::selectService(const std::string& key, const std::string& owner_zid,
                                      std::string& error)
{
    services_->refresh();
    if (!services_->select(key, owner_zid))
    {
        error = "no advertised service '" + key + "'" +
                (owner_zid.empty() ? std::string() : " offered by " + owner_zid) +
                ". switchboard.services lists what is callable.";
        return false;
    }
    return true;
}

void SwitchboardWindow::onServiceSelected()
{
    current_ = services_->selected();
    if (!current_)
    {
        return;
    }

    const auto schema = pub_sub::get_schema(current_->request_schema);
    form_->setSchema(schema ? std::optional(schema->asStruct()) : std::nullopt);

    // Coming back to a service puts its last request back: the usual thing to
    // do next is change one field and send it again.
    const auto past = history_->forKey(current_->key);
    if (!past.empty() && schema)
    {
        std::vector<std::string> ignored;
        form_->setValue(past.front()->request, ignored);
    }

    history_->showKey(current_->key);

    if (past.empty())
    {
        response_->clear("No calls to this service yet.");
    }
    else if (!past.front()->complete)
    {
        response_->showPending(QString::fromStdString(current_->key));
    }
    else
    {
        response_->showResult(past.front()->result,
                              "Last call, at " + past.front()->started.toString("HH:mm:ss"));
    }

    showServiceHeader();
    updateSubmitState();
}

void SwitchboardWindow::onServicesChanged()
{
    if (!current_)
    {
        return;
    }
    // The list keeps its selected row current, and reachability is exactly
    // what changes under a service somebody is looking at.
    current_ = services_->selected();
    showServiceHeader();
    updateSubmitState();
}

void SwitchboardWindow::showServiceHeader()
{
    if (!current_)
    {
        title_->setText("No service selected");
        schemas_->setText("Pick one from the list on the left.");
        doc_->hide();
        warning_->hide();
        return;
    }

    title_->setText(QString::fromStdString(current_->key));
    schemas_->setText(QString::fromStdString(current_->request_schema + " → " +
                                             current_->response_schema + "  ·  offered by " +
                                             ServiceList::ownerLabel(*current_)));

    const auto schema = pub_sub::get_schema(current_->request_schema);
    const std::string_view doc =
        schema ? pub_sub::schema_doc(schema->getProto().getId()) : std::string_view{};
    doc_->setText(QString::fromUtf8(doc.data(), static_cast<qsizetype>(doc.size())));
    doc_->setVisible(!doc.empty());

    QStringList warnings;
    if (!schema)
    {
        warnings << QString("Request schema '%1' is not in this build's registry, so there is no "
                            "form for it. A build with that schema can call it.")
                        .arg(QString::fromStdString(current_->request_schema));
    }
    if (!current_->reachable)
    {
        warnings << "Its node has gone away. Submitting is still allowed -- the advertisement "
                    "may be stale -- but expect no reply.";
    }
    if (current_->offered_by > 1)
    {
        warnings << QString("Offered by %1 nodes. A call reaches all of them, and each "
                            "one's reply is shown.")
                        .arg(current_->offered_by);
    }
    warning_->setText(warnings.join("\n"));
    warning_->setVisible(!warnings.isEmpty());
}

void SwitchboardWindow::updateSubmitState()
{
    const bool has_form = current_ && form_->hasSchema();
    submit_->setEnabled(has_form && form_->isValid() && !callInFlight());
    submit_->setText(callInFlight() ? "Calling…" : "Submit");
    reset_->setEnabled(has_form);

    if (has_form && !form_->isValid())
    {
        QStringList lines;
        for (const std::string& problem : form_->problems())
        {
            if (static_cast<std::size_t>(lines.size()) == kProblemsShown)
            {
                lines << QString("…and %1 more").arg(form_->problems().size() - kProblemsShown);
                break;
            }
            lines << QString::fromStdString(problem);
        }
        form_problems_->setText(lines.join("\n"));
        form_problems_->show();
    }
    else
    {
        form_problems_->hide();
    }
}

int SwitchboardWindow::submit(std::optional<int> timeout_ms, std::string& error)
{
    if (!current_)
    {
        error = "no service is selected.";
        return -1;
    }
    if (!form_->hasSchema())
    {
        error = "request schema '" + current_->request_schema +
                "' is not in this build's registry, so no request can be built.";
        return -1;
    }
    if (!form_->isValid())
    {
        error = "the form is not a valid request:";
        for (const std::string& problem : form_->problems())
        {
            error += " " + problem + ";";
        }
        return -1;
    }
    if (callInFlight())
    {
        error = "a call is already in flight; wait for it to finish.";
        return -1;
    }

    const int timeout = timeout_ms.value_or(timeoutMs());
    if (timeout <= 0)
    {
        error = "timeout_ms must be positive.";
        return -1;
    }

    const int call_id = next_call_id_++;

    CallRecord record;
    record.call_id = call_id;
    record.started = QDateTime::currentDateTime();
    record.key = current_->key;
    record.owner_zid = current_->owner_zid;
    record.request_schema = current_->request_schema;
    record.response_schema = current_->response_schema;
    record.request = form_->value();
    record.timeout = std::chrono::milliseconds(timeout);

    pub_sub::ServiceCallRequest request;
    request.key = record.key;
    request.request_schema = record.request_schema;
    request.response_schema = record.response_schema;
    request.fields = record.request;
    request.timeout = record.timeout;
    request.decode_options.data_hex_limit = kReplyHexLimit;

    history_->add(std::move(record));
    inflight_call_id_ = call_id;
    response_->showPending(QString::fromStdString(current_->key));
    updateSubmitState();

    const std::weak_ptr<Liveness> weak = liveness_;
    pub_sub::callService(
        std::move(request),
        [weak, call_id](pub_sub::ServiceCallResult result)
        {
            // Usually a zenoh thread; the calling thread when nothing was sent.
            // Either way the window is only touched from its own thread.
            const auto liveness = weak.lock();
            if (!liveness)
            {
                return;
            }
            const std::lock_guard<std::mutex> guard(liveness->mutex);
            if (liveness->window == nullptr)
            {
                return;
            }
            auto shared = std::make_shared<pub_sub::ServiceCallResult>(std::move(result));
            SwitchboardWindow* window = liveness->window;
            QMetaObject::invokeMethod(
                window, [window, call_id, shared]() { window->onCallFinished(call_id, *shared); },
                Qt::QueuedConnection);
        });

    return call_id;
}

void SwitchboardWindow::onCallFinished(int call_id, pub_sub::ServiceCallResult result)
{
    const CallRecord* record = history_->complete(call_id, std::move(result));
    if (call_id == inflight_call_id_)
    {
        inflight_call_id_ = 0;
    }

    // Recorded either way; shown only if that service is still the one on
    // screen, so a slow reply does not replace what someone else is reading.
    if (record != nullptr && current_ && record->key == current_->key)
    {
        response_->showResult(record->result);
    }
    updateSubmitState();
}

void SwitchboardWindow::onHistoryActivated(int call_id)
{
    const CallRecord* record = history_->find(call_id);
    if (record == nullptr)
    {
        return;
    }

    std::vector<std::string> ignored;
    form_->setValue(record->request, ignored);

    if (record->complete)
    {
        response_->showResult(record->result, "From history, " + record->started.toString("HH:mm:ss"));
    }
    else
    {
        response_->showPending(QString::fromStdString(record->key));
    }
}

}  // namespace switchboard
