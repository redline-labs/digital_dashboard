#include "pub_sub/raw_subscriber.h"

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/detail/byte_subscriber.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace pub_sub
{

struct RawSubscriber::Impl
{
    // Exactly one of these is set, chosen by which constructor was used.
    Handler handler;
    InfoHandler info_handler;

    // Declared last so it is destroyed FIRST. zenoh's undeclare joins in-flight
    // callbacks, so by the time `handler` is destroyed no callback can still be
    // calling it. Getting this backwards is a use-after-free on every teardown,
    // which is how ZenohExpressionSubscriber learned the same lesson -- do not
    // move it.
    std::unique_ptr<detail::ByteSubscriber> subscriber;
};

RawSubscriber::RawSubscriber(const std::string& keyexpr, Handler on_sample) :
    impl_(std::make_unique<Impl>())
{
    impl_->handler = std::move(on_sample);

    Impl* const impl = impl_.get();
    impl_->subscriber = std::make_unique<detail::ByteSubscriber>(
        keyexpr,
        [impl](const std::vector<std::uint8_t>& payload, const detail::SampleMeta& meta) {
            if (!impl->handler)
            {
                return;
            }

            // encoding() copies out of zenoh, so it is called once per sample
            // and only because the schema name is the whole point of this
            // class. The view handed to the handler is over this local, which
            // is why the contract says copy it to keep it.
            const std::string encoding = meta.encoding();
            impl->handler(payload, schemaNameFromEncoding(encoding));
        });

    // Unlike the expression subscriber there is no startup race to guard here:
    // the handler is installed before the subscription is declared, so it is
    // never read while being written.
}

RawSubscriber::RawSubscriber(const std::string& keyexpr, InfoHandler on_sample) :
    impl_(std::make_unique<Impl>())
{
    impl_->info_handler = std::move(on_sample);

    Impl* const impl = impl_.get();
    impl_->subscriber = std::make_unique<detail::ByteSubscriber>(
        keyexpr,
        [impl](const std::vector<std::uint8_t>& payload, const detail::SampleMeta& meta) {
            if (!impl->info_handler)
            {
                return;
            }

            // Both of these copy out of zenoh; the locals are what the views in
            // SampleInfo point at, so they have to outlive the handler call and
            // therefore live here rather than inside the aggregate.
            const std::string encoding = meta.encoding();
            const std::string origin_zid = meta.originZid();

            const SampleInfo info{
                .keyexpr = meta.keyexpr(),
                .schema_name = schemaNameFromEncoding(encoding),
                .publish_time_nanos = meta.publishTimeNanos(),
                .origin_zid = origin_zid,
            };

            impl->info_handler(payload, info);
        });
}

RawSubscriber::~RawSubscriber() = default;

bool RawSubscriber::isValid() const
{
    return impl_->subscriber && impl_->subscriber->isValid();
}

std::string_view RawSubscriber::keyexpr() const
{
    return impl_->subscriber ? impl_->subscriber->keyexpr() : std::string_view{};
}

}  // namespace pub_sub
