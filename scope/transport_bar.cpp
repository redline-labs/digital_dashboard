#include "scope/transport_bar.h"

#include "scope/capture_buffer.h"
#include "scope/overview_controller.h"
#include "scope/overview_strip.h"
#include "scope/recorded_source.h"
#include "scope/scope_recorder.h"
#include "scope/scope_window.h"
#include "scope/time_base.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QToolBar>
#include <QToolButton>

namespace scope
{

namespace
{

// The playback rates the combo offers. Both ends earn their place: 0.1x to
// study a transient you have already found, 20x to find one.
constexpr double kRates[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0};

QString formatWallClock(std::uint64_t unix_nanos)
{
    if (unix_nanos == 0)
    {
        return {};
    }
    const QDateTime when =
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(unix_nanos / 1'000'000ull));
    return when.toString(QStringLiteral("HH:mm:ss"));
}

}  // namespace

void TransportBar::build()
{
    ScopeWindow* const window = &window_;

    // The overview goes in a bar of its OWN, above the controls, so it gets the
    // full width. Sharing a row with the buttons would leave it a few hundred
    // pixels for a whole recording, which is the resolution the QSlider it
    // replaced had and the reason that slider was useless for finding anything.
    auto* overview_bar = new QToolBar(QObject::tr("Overview"), window);
    overview_bar->setObjectName("overview_bar");
    overview_bar->setMovable(false);
    window->addToolBar(Qt::BottomToolBarArea, overview_bar);

    window_.overview_ = new OverviewStrip(overview_bar);
    overview_bar->addWidget(window_.overview_);
    // Stretches to the bar's width rather than sitting at its natural size,
    // which for a custom widget in a toolbar is the minimum.
    window_.overview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QObject::connect(window_.overview_, &OverviewStrip::viewRequested, window,
                     [window](double begin, double end) {
                         if (!window->updating_transport_)
                         {
                             window->time_base_->setView(begin, end);
                         }
                     });
    QObject::connect(window_.overview_, &OverviewStrip::cursorRequested, window,
                     [window](std::optional<double> t) { window->time_base_->setCursor(t); });

    window->addToolBarBreak(Qt::BottomToolBarArea);

    auto* bar = new QToolBar(QObject::tr("Transport"), window);
    bar->setObjectName("transport_bar");
    bar->setMovable(false);
    window->addToolBar(Qt::BottomToolBarArea, bar);

    // ------------------------------------------------------------------ live

    pause_button_ = new QToolButton(bar);
    pause_button_->setObjectName("transport_pause");
    pause_button_->setCheckable(true);
    pause_button_->setText(QObject::tr("Pause"));
    // Sets the mode and NOTHING ELSE. The label is update()'s, because
    // following can be turned off by a pan or a zoom that never comes through
    // here -- so a handler that also wrote the text would be one of two authors
    // for one label, which is how it ended up with three different words for two
    // states.
    QObject::connect(pause_button_, &QToolButton::toggled, window, [window](bool paused) {
        if (!window->updating_transport_)
        {
            window->time_base_->setMode(paused ? TimeBase::Mode::Paused : TimeBase::Mode::Live);
        }
    });
    live_controls_.push_back(bar->addWidget(pause_button_));

    // ---------------------------------------------------------------- review

    const auto add_button = [&](const char* name, const QString& text, const QString& tip,
                                bool checkable, auto&& on_click) {
        auto* button = new QToolButton(bar);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(checkable);
        if (checkable)
        {
            QObject::connect(button, &QToolButton::toggled, window, on_click);
        }
        else
        {
            QObject::connect(button, &QToolButton::clicked, window, on_click);
        }
        review_controls_.push_back(bar->addWidget(button));
        return button;
    };

    add_button("transport_to_start", QObject::tr("|◀"),
               QObject::tr("Jump to the start of the recording"), false,
               [window]() { window->time_base_->seek(window->source().caps().t_begin); });

    add_button("transport_step_back", QObject::tr("◀"), QObject::tr("Back one second"), false,
               [window]() { window->time_base_->seek(window->source().now() - 1.0); });

    play_button_ = add_button("transport_play", QObject::tr("▶"), QObject::tr("Play"), true,
                              [window](bool playing) { window->time_base_->setPlaying(playing); });

    add_button("transport_step_forward", QObject::tr("▶"), QObject::tr("Forward one second"),
               false, [window]() { window->time_base_->seek(window->source().now() + 1.0); });

    add_button("transport_to_end", QObject::tr("▶|"),
               QObject::tr("Jump to the end of the recording"), false,
               [window]() { window->time_base_->seek(window->source().caps().t_end); });

    rate_combo_ = new QComboBox(bar);
    rate_combo_->setObjectName("transport_rate");
    for (const double rate : kRates)
    {
        rate_combo_->addItem(QStringLiteral("%1x").arg(rate), rate);
    }
    rate_combo_->setCurrentIndex(3);  // 1x
    QObject::connect(rate_combo_, &QComboBox::currentIndexChanged, window,
                     [window, this](int index) {
                         if (index >= 0)
                         {
                             window->time_base_->setRate(rate_combo_->itemData(index).toDouble());
                         }
                     });
    review_controls_.push_back(bar->addWidget(rate_combo_));

    // --------------------------------------------------------------- shared

    bar->addSeparator();
    auto* window_label = new QLabel(QObject::tr("  Window "), bar);
    bar->addWidget(window_label);

    // Shared, not live-only. What span the plot shows matters at least as much
    // when reviewing a recording as when tailing the bus.
    window_spin_ = new QDoubleSpinBox(bar);
    window_spin_->setObjectName("transport_window_seconds");
    window_spin_->setRange(0.1, 3600.0);
    window_spin_->setDecimals(1);
    window_spin_->setSingleStep(5.0);
    window_spin_->setSuffix(QObject::tr(" s"));
    window_spin_->setValue(window_.time_base_->windowSeconds());
    QObject::connect(window_spin_, &QDoubleSpinBox::valueChanged, window,
                     [window](double seconds) {
                         if (!window->updating_transport_)
                         {
                             window->time_base_->setWindowSeconds(seconds);
                         }
                     });
    bar->addWidget(window_spin_);

    bar->addSeparator();
    cursor_label_ = new QLabel(bar);
    cursor_label_->setObjectName("transport_cursor");
    cursor_label_->setMinimumWidth(200);
    bar->addWidget(cursor_label_);

    transport_status_ = new QLabel(bar);
    transport_status_->setObjectName("transport_status");
    transport_status_->setStyleSheet("color: palette(mid); font-size: 11px;");
    bar->addWidget(transport_status_);

    QObject::connect(window_.time_base_.get(), &TimeBase::cursorMoved, window,
                     [this]() { update(); });
    QObject::connect(window_.time_base_.get(), &TimeBase::changed, window,
                     [this]() { update(); });

    // The one render timer drives the readout too. A second timer for the
    // transport bar would tick against the panels' and make the position
    // readout disagree with the line beside it.
    QObject::connect(window_.time_base_.get(), &TimeBase::frame, window,
                     [this]() { update(); });
}

void TransportBar::applyCaps(const SourceCaps& caps)
{
    for (QAction* action : live_controls_)
    {
        action->setVisible(caps.live);
    }
    for (QAction* action : review_controls_)
    {
        action->setVisible(caps.seekable);
    }

    if (play_button_ != nullptr)
    {
        play_button_->setChecked(false);
    }

    // The time base reset its rate to 1.0x on the swap; the combo has to
    // follow, or it shows a multiplier the source is not honouring.
    if (rate_combo_ != nullptr)
    {
        const int one_x = rate_combo_->findData(1.0);
        if (one_x >= 0)
        {
            rate_combo_->setCurrentIndex(one_x);
        }
    }
}

void TransportBar::update()
{
    if (cursor_label_ == nullptr)
    {
        return;
    }

    const SourceCaps caps = window_.source_->caps();
    const double position = window_.source_->now();
    TimeBase& time_base = *window_.time_base_;

    const bool was_updating = window_.updating_transport_;
    window_.updating_transport_ = true;

    if (window_spin_ != nullptr && window_spin_->value() != time_base.windowSeconds())
    {
        window_spin_->setValue(time_base.windowSeconds());
    }

    if (play_button_ != nullptr && play_button_->isChecked() != time_base.playing())
    {
        // Playback stops itself at the end of the recording, and the button has
        // to follow or it claims to still be playing.
        play_button_->setChecked(time_base.playing());
    }

    // The cursor if there is one, the playback head otherwise. A recording also
    // has a wall clock, which is the one thing a bag genuinely knows and the
    // live source does not -- so it goes here rather than on the axis, whose
    // relative labels ("-10 s") are what someone reading a trace wants.
    const std::optional<double>& cursor = time_base.cursor();
    const double t = cursor ? *cursor : position;

    QString text = QObject::tr("  t = %1 s").arg(t, 0, 'f', 3);
    if (!caps.live)
    {
        if (const auto* recorded = dynamic_cast<const RecordedSource*>(window_.source_.get()))
        {
            const QString wall = formatWallClock(recorded->wallClockNanosAt(t));
            if (!wall.isEmpty())
            {
                text += QStringLiteral("  (%1)").arg(wall);
            }
        }
    }
    cursor_label_->setText(text);

    // The capture's state. A capture whose head is being evicted is the same
    // class of thing as a recorder dropping samples: the part of the session you
    // can still review has a boundary, and it moves. Saying so is what stops
    // someone scrubbing back into a gap and reading it as a publisher that had
    // not started.
    //
    // ONE widget shows this. It used to be written to the top bar's chip as
    // well, character for character, from these same lines -- and a string
    // rendered twice in one window is a tell that one of the two has no job of
    // its own. The chip describes the SOURCE, which is a different question.
    if (transport_status_ != nullptr)
    {
        QString state;

        // Decodes still running over a freshly opened recording. Without this,
        // opening a bag with an eight-trace workspace shows eight empty plots
        // for as long as the decode takes -- indistinguishable from a bag that
        // does not contain those signals. The agent reads the same fact from
        // scope.source's decodes_pending; this is the human's copy.
        if (const auto* recorded = dynamic_cast<const RecordedSource*>(window_.source_.get()))
        {
            if (const std::size_t pending = recorded->decodesPending(); pending > 0)
            {
                state += QObject::tr("  ⏳ decoding %n signal(s)…", nullptr,
                                     static_cast<int>(pending));
            }
        }

        if (window_.recorder_ != nullptr)
        {
            // One lock acquisition for all four numbers. This runs per render
            // tick, and taking the capture's mutex four separate times was four
            // chances per frame to contend with the RX thread for one answer.
            const CaptureBuffer::Stats capture = window_.recorder_->buffer().stats();
            state += window_.isOnline()
                         ? QObject::tr("  ⏺ %1 s captured")
                               .arg(capture.retained_span_seconds, 0, 'f', 0)
                         : QObject::tr("  ⏹ %1 s captured")
                               .arg(capture.retained_span_seconds, 0, 'f', 0);
            if (capture.evicted > 0)
            {
                state += QObject::tr(", %1 evicted").arg(capture.evicted);
            }
            if (capture.messages > 0 && capture.revision != window_.capture_saved_revision_)
            {
                state += QObject::tr(" (unsaved)");
            }
        }
        transport_status_->setText(state);
    }

    window_.updateSourceChip();

    // ONE label pair, driven from ONE place.
    //
    // The button used to be written from here AND from its own toggled()
    // handler, with different words: this one says Pause/Follow, that one said
    // Pause/Live. Which of the three you saw depended on how the state had last
    // been reached, so the same frozen plot could be sitting under a button
    // marked "Live" or one marked "Follow".
    //
    // Reading it back from the time base is the part that has to stay: a pan or
    // a zoom turns following off without touching the button, and a button left
    // to its own toggled() sits there saying "Pause" over a plot that has
    // stopped scrolling.
    if (pause_button_ != nullptr)
    {
        // Labelled with the STATE it is in, exactly like the mode toggle and
        // for the documented reason: a checkable control named for its action
        // has to be read together with its checked state to know which way
        // round it is, and half the people reading it get that wrong. This one
        // said "Pause" while following and "Follow" while paused -- the label
        // and the checkmark moving in opposite directions.
        const bool following = time_base.following();
        pause_button_->setChecked(!following);
        pause_button_->setText(following ? QObject::tr("▶ Following")
                                         : QObject::tr("⏸ Paused"));
        pause_button_->setToolTip(
            following ? QObject::tr("The view is scrolling with the source. Click to "
                                    "freeze it.")
                      : QObject::tr("The view is frozen. Click to follow again."));
    }

    window_.overview_controller_->updateOverview();

    window_.updating_transport_ = was_updating;
}

}  // namespace scope
