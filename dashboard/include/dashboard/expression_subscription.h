#ifndef DASHBOARD_EXPRESSION_SUBSCRIPTION_H_
#define DASHBOARD_EXPRESSION_SUBSCRIPTION_H_

#include <functional>
#include <memory>
#include <string>

#include <QMetaObject>
#include <QObject>

#include <spdlog/spdlog.h>

#include "pub_sub/zenoh_subscriber.h"
#include "reflection/reflection.h"

namespace dashboard {

// Builds a ZenohExpressionSubscriber for `expression`, validates it, and
// delivers evaluated results of type T to `setter` on `receiver`'s thread via
// a queued invocation (safe to call from the Zenoh RX thread). Returns
// nullptr, with the error logged under `log_context`, if construction or
// validation fails. `setter` is a member-function pointer of Receiver (or any
// callable invocable as setter(receiver, value)).
template <typename T, typename Receiver, typename Setter>
std::unique_ptr<pub_sub::ZenohExpressionSubscriber> makeExpressionSubscription(
    pub_sub::schema_type_t schema_type,
    const std::string& expression,
    const std::string& zenoh_key,
    Receiver* receiver,
    Setter setter,
    const char* log_context)
{
    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> subscriber;
    try
    {
        subscriber = std::make_unique<pub_sub::ZenohExpressionSubscriber>(schema_type, expression, zenoh_key);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}: failed to initialize expression subscriber: {}", log_context, e.what());
        return nullptr;
    }

    if (!subscriber->isValid())
    {
        SPDLOG_ERROR("{}: invalid expression '{}' for schema '{}'", log_context, expression,
                     reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema_type));
        return nullptr;
    }

    subscriber->setResultCallback<T>([receiver, setter](T value) {
        QMetaObject::invokeMethod(
            receiver,
            [receiver, setter, value]() { std::invoke(setter, receiver, value); },
            Qt::QueuedConnection);
    });

    return subscriber;
}

}  // namespace dashboard

#endif  // DASHBOARD_EXPRESSION_SUBSCRIPTION_H_
