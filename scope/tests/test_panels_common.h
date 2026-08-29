// SPDX-License-Identifier: GPL-3.0-or-later
//
#ifndef SCOPE_TEST_PANELS_COMMON_H_
#define SCOPE_TEST_PANELS_COMMON_H_

// Shared plumbing for the scope_test_panels suites: the expect() harness with
// its cross-file counters, the stub sources, and the binding-candidate
// factories. One executable, one bus-free stub world; the suites live one per
// file and register through a run<Area>Tests() entry each -- see
// test_panels.cpp for the roster.

#include "scope/data_source.h"

#include "pub_sub/session_manager.h"
#include "scope/overview_strip.h"
#include "scope/panel_config_dialog.h"
#include "scope/panel_registry.h"
#include "scope/scope_window.h"
#include "scope/signal_browser.h"
#include "scope/time_base.h"

#include "map_panel/map_panel.h"
#include "table/table_panel.h"
#include "time_series/time_series_panel.h"
#include "video/video_panel.h"
#include "video/video_scrubber.h"

#include "carplay_video.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "config_codec/config_json.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace panel_tests
{

inline int failures = 0;
inline int checks = 0;

inline void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A DataSource that binds anything and produces nothing, so panel behaviour can
// be tested without a bus. Binding succeeding is what the panel cares about;
// where the samples come from is LiveZenohSource's problem and is covered by
// the ring and evaluator tests.
class StubSource : public scope::DataSource
{
  public:
    scope::SourceCaps caps() const override
    {
        scope::SourceCaps caps;
        caps.live = true;
        caps.seekable = false;
        return caps;
    }

    std::vector<scope::TopicInfo> topics() const override { return available; }
    std::uint64_t topicsRevision() const override { return revision; }

    scope::SignalHandle bind(const scope::SignalKey& key,
                             std::shared_ptr<scope::SignalBuffer> into) override
    {
        bound.push_back(key);
        buffers.push_back(std::move(into));
        return next_handle++;
    }

    void release(scope::SignalHandle handle) override { released.push_back(handle); }

    // The raw seam, stubbed the same way and for the same reason. A video panel
    // binding successfully is what the panel layer cares about; what arrives is
    // the source's problem and is covered elsewhere.
    scope::RawHandle bindRaw(const std::string& zenoh_key,
                             pub_sub::schema_type_t schema,
                             std::shared_ptr<scope::RawBuffer> into,
                             scope::RawClassifier classify) override
    {
        static_cast<void>(classify);
        bound_raw.push_back({zenoh_key, schema});
        raw_buffers.push_back(std::move(into));
        return scope::RawHandle{next_handle++};
    }

    void releaseRaw(scope::RawHandle handle) override { released_raw.push_back(handle); }

    double now() const override { return 100.0; }

    std::vector<scope::TopicInfo> available;
    std::uint64_t revision = 0;
    std::vector<scope::SignalKey> bound;
    std::vector<std::shared_ptr<scope::SignalBuffer>> buffers;
    std::vector<scope::SignalHandle> released;

    std::vector<std::pair<std::string, pub_sub::schema_type_t>> bound_raw;
    std::vector<std::shared_ptr<scope::RawBuffer>> raw_buffers;
    std::vector<scope::RawHandle> released_raw;

    scope::SignalHandle next_handle = 1;
};

// A stub that reports itself as a recording, for the parts of the window that
// render from caps() rather than from what the source actually contains.
class SeekableStub : public StubSource
{
  public:
    scope::SourceCaps caps() const override
    {
        scope::SourceCaps caps;
        caps.live = false;
        caps.seekable = true;
        caps.t_begin = 0.0;
        caps.t_end = 120.0;
        return caps;
    }

    void seek(double t) override { seeks.push_back(t); }

    std::vector<double> seeks;
};

inline scope::BindingCandidate numericField()
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = "vehicle/engine/rpm";
    candidate.schema_name = "EngineRpm";
    candidate.field_name = "rpm";
    candidate.type_category = "uint";
    return candidate;
}

// The whole video topic, which is what the browser produces for a topic row.
// `field_name` empty is what makes it topic-level.
inline scope::BindingCandidate videoTopic()
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = "nodes/carplay/video";
    candidate.schema_name = "CarPlayVideo";
    return candidate;
}

// A field OF the video topic. A plot will take this; the video panel must not,
// because a panel that accepted it would bind the topic and then render
// whatever "isKeyframe" happened to mean as a picture.
inline scope::BindingCandidate videoField()
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = "nodes/carplay/video";
    candidate.schema_name = "CarPlayVideo";
    candidate.field_name = "widthPx";
    candidate.type_category = "uint";
    return candidate;
}

// A topic-level candidate for a schema the video panel knows nothing about.
inline scope::BindingCandidate otherTopic()
{
    scope::BindingCandidate candidate;
    candidate.zenoh_key = "vehicle/engine/rpm";
    candidate.schema_name = "EngineRpm";
    return candidate;
}


// ------------------------------------------------------------- the table panel

// Push samples into the buffer the panel bound and drain them into the history
// the readout reads. `now` is the instant the retention trim measures back from,
// which for StubSource is 100.0 -- so samples live in the high nineties.
inline void feed(StubSource& source, std::size_t index, const std::vector<scope::Sample>& samples,
          double now = 100.0)
{
    if (index >= source.buffers.size())
    {
        expect(false, "feed: the panel did not bind that row");
        return;
    }
    for (const scope::Sample& sample : samples)
    {
        source.buffers[index]->push(sample);
    }
    source.buffers[index]->drain(now);
}



inline table_row_t rpmRow()
{
    table_row_t row;
    row.zenoh_key = "vehicle/engine/rpm";
    row.schema_type = pub_sub::schema_type_t::EngineRpm;
    row.value_expression = "rpm";
    row.label = "rpm";
    row.units = "rpm";
    return row;
}


// A panel big enough to have a usable plot rect. Below the gutters plotRect()
// returns its 1x1 degenerate guard and every gesture is correctly a no-op --
// which would make these tests pass while proving nothing.
// paintEvent is what fills drawn_begin_/drawn_end_, and every gesture maps
// against those rather than against the time base -- deliberately, so a click
// lands on the instant the user can see. The consequence for a test is that
// moving the view and then sending a gesture without a repaint in between
// converts against the PREVIOUS window. Rendering into a pixmap is the only way
// to force a paint with no compositor.
inline void forcePaint(QWidget* panel)
{
    QPixmap scratch(panel->size());
    panel->render(&scratch);
}



inline scope::TimeSeriesPanel* readyPanel(scope::ScopeWindow& window, const QString& id)
{
    window.addPanel(scope::panel_type_t::time_series, id);
    auto* panel = static_cast<scope::TimeSeriesPanel*>(window.findPanel(id)->panel);
    panel->resize(600, 400);
    forcePaint(panel);
    return panel;
}


inline void mouse(QWidget* target, QEvent::Type type, QPointF at, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent event(type, at, target->mapToGlobal(at), button, buttons, mods);
    QCoreApplication::sendEvent(target, &event);
}

// One entry per suite file, called in order by main().
void runRegistryTests();
void runPlotTests();
void runTableTests();
void runWindowTests();
void runGestureTests();
void runChromeTests();
void runMapPanelTests();

}  // namespace panel_tests

#endif  // SCOPE_TEST_PANELS_COMMON_H_
