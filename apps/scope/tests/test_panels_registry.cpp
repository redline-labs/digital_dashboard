// SPDX-License-Identifier: GPL-3.0-or-later
//
#include "test_panels_common.h"

// The panel table and everything generated from it: construction,
// config/stats variants, binding acceptance, the video decoder feed, and the
// reflected config dialog.

namespace panel_tests
{

// ----------------------------------------------------------------- the registry

void testThePanelTableDrivesEverything()
{
    const std::vector<scope::PanelTypeInfo> types = scope::availablePanelTypes();
    expect(!types.empty(), "at least one panel type is registered");

    bool found_time_series = false;
    for (const scope::PanelTypeInfo& info : types)
    {
        if (info.type == scope::panel_type_t::time_series)
        {
            found_time_series = true;
            expect(info.name == "time_series", "the enum name comes from the table");
            expect(info.friendly_name == "Time Series", "the friendly name comes from the class");
        }
    }
    expect(found_time_series, "the time-series panel is in the registry");

    bool found_video = false;
    for (const scope::PanelTypeInfo& info : types)
    {
        if (info.type == scope::panel_type_t::video)
        {
            found_video = true;
            expect(info.name == "video", "the video panel's enum name comes from the table");
            expect(info.friendly_name == "Video", "its friendly name comes from the class");
        }
    }
    expect(found_video, "the video panel is in the registry");

    bool found_table = false;
    for (const scope::PanelTypeInfo& info : types)
    {
        if (info.type == scope::panel_type_t::table)
        {
            found_table = true;
            expect(info.name == "table", "the table panel's enum name comes from the table");
            expect(info.friendly_name == "Table", "its friendly name comes from the class");
        }
    }
    expect(found_table, "the table panel is in the registry");

    // Every type has a glyph, and no two share one -- a blank or duplicated
    // toolbar button reads as a broken app rather than an unregistered panel.
    for (const scope::PanelTypeInfo& info : types)
    {
        expect(!info.toolbar_glyph.empty(), "every panel type has a toolbar glyph");
    }
    for (std::size_t i = 0; i < types.size(); ++i)
    {
        for (std::size_t j = i + 1; j < types.size(); ++j)
        {
            expect(types[i].toolbar_glyph != types[j].toolbar_glyph,
                   "no two panel types share a toolbar glyph");
        }
    }
}

// THE SEAM ITSELF, driven off the table rather than off a list of types written
// here. This is what keeps the NEXT panel honest: it fails the moment someone
// adds a table row whose class does not carry a stats struct, or whose variants
// do not line up with its enumerator.
//
// The failure it exists to prevent is not a crash. `scope.stats` would answer
// `{}` for the new panel, and an empty stats object looks exactly like a panel
// that is working and has received nothing.
void testEveryPanelTypeServesItsConfigAndStats()
{
    StubSource source;

    for (const scope::PanelTypeInfo& info : scope::availablePanelTypes())
    {
        const std::string name(info.name);

        const scope::panel_config_variant_t defaults = scope::default_panel_config(info.type);
        expect(!std::holds_alternative<std::monostate>(defaults),
               name + ": default_panel_config produces a real alternative");
        expect(scope::panelTypeOf(defaults) == info.type,
               name + ": the type round-trips through the config variant");

        std::unique_ptr<scope::Panel> panel = scope::createPanel(defaults, source);
        expect(panel != nullptr, name + ": the registry can construct one");
        if (panel == nullptr)
        {
            continue;
        }

        expect(panel->panelType() == info.type, name + ": it reports its own type");

        const scope::panel_config_variant_t live = scope::panelConfigOf(*panel);
        expect(!std::holds_alternative<std::monostate>(live),
               name + ": panelConfigOf returns the panel's own config, not monostate");
        expect(scope::panelTypeOf(live) == info.type,
               name + ": the config it returns is the right kind");
        expect(scope::applyPanelConfig(*panel, live),
               name + ": the config it returns is one it will accept back");

        const scope::panel_stats_variant_t stats = scope::panelStatsOf(*panel);
        expect(!std::holds_alternative<std::monostate>(stats),
               name + ": panelStatsOf returns a real stats struct");

        // And both survive the reflected codec the agent interface serves them
        // through, which is where an unsupported field type would surface.
        std::visit(
            [&name](const auto& value) {
                using value_t = std::decay_t<decltype(value)>;
                if constexpr (!std::is_same_v<value_t, std::monostate>)
                {
                    expect(config_codec::toJson(value).is_object(),
                           name + ": its stats serialise to a JSON object");
                    expect(config_codec::describeType<value_t>().is_object(),
                           name + ": its stats describe themselves");
                }
            },
            stats);
    }
}

// The two panels are mirror images at the binding seam, and neither the browser
// nor the drag plumbing knows either exists. Every case below is one the drop
// path has to get right without asking what kind of panel it is holding.
void testBindingAcceptanceIsMirrored()
{
    StubSource source;

    std::unique_ptr<scope::Panel> plot =
        scope::createPanel(scope::default_panel_config(scope::panel_type_t::time_series), source);
    std::unique_ptr<scope::Panel> video =
        scope::createPanel(scope::default_panel_config(scope::panel_type_t::video), source);
    expect(plot != nullptr && video != nullptr, "both panel kinds constructed");
    if (plot == nullptr || video == nullptr)
    {
        return;
    }

    expect(plot->acceptsBinding(numericField()), "a plot takes a numeric field");
    expect(!plot->acceptsBinding(videoTopic()), "a plot declines a whole topic");

    expect(video->acceptsBinding(videoTopic()), "the video panel takes the video TOPIC");
    expect(!video->acceptsBinding(videoField()),
           "the video panel declines a FIELD of the video topic");
    expect(!video->acceptsBinding(numericField()),
           "the video panel declines a numeric field");
    expect(!video->acceptsBinding(otherTopic()),
           "the video panel declines a topic of another schema");

    // One stream per panel: a second is declined rather than silently replacing
    // what is bound.
    expect(video->addBinding(videoTopic()), "the video panel takes its first stream");
    expect(!video->acceptsBinding(videoTopic()),
           "and declines a second rather than replacing the first");

    expect(!source.bound_raw.empty(), "binding reached the source's raw seam");
    if (!source.bound_raw.empty())
    {
        expect(source.bound_raw.front().first == "nodes/carplay/video",
               "it bound the key it was given");
        expect(source.bound_raw.front().second == pub_sub::schema_type_t::CarPlayVideo,
               "under the CarPlayVideo schema -- note the capital P");
    }
}

// The ordering rule, for raw handles. A handle means nothing to a source that
// did not issue it, so a panel must release against the OLD source before it
// repoints -- and the window destroys the old source only after this returns,
// precisely so the release has somewhere to go.
void testTheDecoderIsFedTheAccessUnitNotTheEnvelope()
{
    // THE BUFFER HOLDS WHOLE CAPNP MESSAGES, and the decoder must be handed the
    // Annex-B access unit inside one rather than the message around it.
    //
    // REGRESSION, and a nasty one: the whole envelope used to go to libavcodec
    // and decoded anyway, because the software H.264 decoder scans for a start
    // code and silently steps over whatever precedes it. Nothing looked wrong
    // for as long as that was the only decoder. A hardware decoder does not do
    // that -- it took the keyframes and rejected every P-frame -- so the panel
    // showed one picture every two seconds and blamed the GPU.
    //
    // This checks the seam directly rather than through a decoder, because a
    // decoder is exactly what papered over it.
    const std::vector<std::uint8_t> access_unit = {0x00, 0x00, 0x00, 0x01, 0x65,
                                                   0xDE, 0xAD, 0xBE, 0xEF};

    capnp::MallocMessageBuilder builder;
    CarPlayVideo::Builder video = builder.initRoot<CarPlayVideo>();
    video.setCodec(CarPlayVideo::Codec::H264);
    video.setIsKeyframe(true);
    video.setData(kj::arrayPtr(access_unit.data(), access_unit.size()));

    const kj::Array<capnp::word> words = capnp::messageToFlatArray(builder);
    const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
    const std::vector<std::uint8_t> payload(bytes.begin(), bytes.end());

    std::vector<std::uint8_t> out;
    const bool read = scope::VideoPanel::accessUnitBytes(payload, out);

    expect(read, "envelope: the message was readable");
    expect(out == access_unit,
           "envelope: the decoder is fed the ACCESS UNIT, byte for byte");
    expect(out.size() < payload.size(),
           "envelope: which is smaller than the message carrying it -- the check "
           "that fails if the whole payload is handed over");

    // And a payload that is not a CarPlayVideo message at all is declined rather
    // than passed through as if it were video.
    std::vector<std::uint8_t> from_rubbish;
    expect(!scope::VideoPanel::accessUnitBytes({1, 2, 3, 4, 5, 6, 7, 8}, from_rubbish),
           "envelope: a payload that is not a CarPlayVideo message is declined");
}

void testVideoPanelReleasesBeforeRepointing()
{
    StubSource first;
    SeekableStub second;

    std::unique_ptr<scope::Panel> video =
        scope::createPanel(scope::default_panel_config(scope::panel_type_t::video), first);
    expect(video != nullptr, "the video panel constructed");
    if (video == nullptr)
    {
        return;
    }

    expect(video->addBinding(videoTopic()), "a stream is bound on the first source");
    expect(first.bound_raw.size() == 1, "the first source issued a raw handle");
    const scope::RawHandle issued{first.next_handle - 1};

    video->rebindTo(second);

    expect(first.released_raw.size() == 1,
           "the handle was released against the source that ISSUED it");
    if (!first.released_raw.empty())
    {
        expect(first.released_raw.front() == issued, "and it was the right handle");
    }
    expect(second.released_raw.empty(),
           "nothing was released against the source that never issued anything");
    expect(second.bound_raw.size() == 1, "the stream was rebound on the new source");
}

void testDefaultConfigMatchesTheType()
{
    const scope::panel_config_variant_t config =
        scope::default_panel_config(scope::panel_type_t::time_series);
    expect(std::holds_alternative<TimeSeriesPanelConfig_t>(config),
           "a default config holds the alternative for its type");
    expect(scope::panelTypeOf(config) == scope::panel_type_t::time_series,
           "the type can be recovered from the config variant");

    const scope::panel_config_variant_t unknown =
        scope::default_panel_config(scope::panel_type_t::unknown);
    expect(std::holds_alternative<std::monostate>(unknown),
           "an unknown type produces monostate rather than some arbitrary panel");
    expect(scope::panelTypeOf(unknown) == scope::panel_type_t::unknown,
           "monostate reports back as unknown");
}

void testCreatingAnUnknownPanelReturnsNull()
{
    StubSource source;
    std::unique_ptr<scope::Panel> panel =
        scope::createPanel(scope::panel_config_variant_t{std::monostate{}}, source);
    expect(panel == nullptr,
           "an unknown panel type constructs nothing, rather than substituting another kind");
}

void testValidateClampsBeforeConstruction()
{
    // The panel must never see a config the loader would have clamped, or it
    // would report the unclamped values back when the workspace was saved.
    StubSource source;
    TimeSeriesPanelConfig_t config;
    config.window_seconds = 1e9;  // Far beyond the cap.
    config.y_min = 100.0;         // Inverted range.
    config.y_max = 0.0;

    std::unique_ptr<scope::Panel> panel = scope::createPanel(config, source);
    expect(panel != nullptr, "a panel with an out-of-range config still constructs");

    auto* plot = qobject_cast<scope::TimeSeriesPanel*>(panel.get());
    expect(plot != nullptr, "it is the right kind of panel");
    if (plot != nullptr)
    {
        expect(plot->getConfig().window_seconds <= 24.0 * 60.0 * 60.0,
               "an oversized window is clamped before the panel sees it");
        expect(plot->getConfig().y_min < plot->getConfig().y_max,
               "an inverted Y range is ordered before the panel sees it");
    }
}

void testApplyConfigClampsLikeALoad()
{
    // The OTHER path into a live panel: scope.panel_set_config (and the config
    // dialog) go through applyPanelConfig, which used to skip validate()
    // entirely. An out-of-range config applied there lived in the panel, was
    // saved to the workspace, and was only clamped back on the next load -- so
    // save/load/save was not a fixed point.
    StubSource source;
    std::unique_ptr<scope::Panel> panel =
        scope::createPanel(TimeSeriesPanelConfig_t{}, source);
    expect(panel != nullptr, "the panel constructed");
    if (panel == nullptr)
    {
        return;
    }

    TimeSeriesPanelConfig_t bad;
    bad.window_seconds = 1e9;  // Far beyond the cap.
    bad.y_min = 100.0;         // Inverted range.
    bad.y_max = 0.0;
    expect(scope::applyPanelConfig(*panel, scope::panel_config_variant_t{bad}),
           "the config was applied");

    auto* plot = qobject_cast<scope::TimeSeriesPanel*>(panel.get());
    if (plot != nullptr)
    {
        expect(plot->getConfig().window_seconds <= 24.0 * 60.0 * 60.0,
               "applyPanelConfig clamps an oversized window exactly as a load would");
        expect(plot->getConfig().y_min < plot->getConfig().y_max,
               "applyPanelConfig orders an inverted Y range exactly as a load would");
    }
}

void runRegistryTests()
{
    testThePanelTableDrivesEverything();
    testDefaultConfigMatchesTheType();
    testCreatingAnUnknownPanelReturnsNull();
    testValidateClampsBeforeConstruction();
    testApplyConfigClampsLikeALoad();
    testEveryPanelTypeServesItsConfigAndStats();
    testBindingAcceptanceIsMirrored();
    testTheDecoderIsFedTheAccessUnitNotTheEnvelope();
    testVideoPanelReleasesBeforeRepointing();
}

}  // namespace panel_tests
