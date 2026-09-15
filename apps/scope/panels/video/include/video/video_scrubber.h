#ifndef SCOPE_VIDEO_SCRUBBER_H_
#define SCOPE_VIDEO_SCRUBBER_H_

#include <QWidget>

#include <optional>
#include <vector>

namespace scope
{

// The video panel's own seek bar.
//
// WHY THE PANEL HAS ONE AT ALL, given the window already has an overview strip:
// so the panel is useful by itself. A video panel floated on a second monitor,
// or the only panel in a workspace, should be scrubbable without the bottom bar
// of the main window being visible -- and the natural place to grab a video's
// timeline is under the video.
//
// IT IS A DUMB PAINTER, exactly like OverviewStrip and for the same reason. It
// knows nothing about TimeBase or DataSource; the panel pushes numbers in and
// connects to what comes out. That keeps "a drag becomes a seek" in one place
// and makes this testable with four setters and a synthesised drag.
//
// It drives the SHARED time base rather than a position of its own. On a
// recorded source the view's right edge IS the playhead -- RecordedSource
// refills every buffer around one position -- so a video panel scrubbing
// independently would draw a frame from one instant beside traces from another,
// and it would look like data rather than like a bug.
class VideoScrubber : public QWidget
{
    Q_OBJECT

  public:
    explicit VideoScrubber(QWidget* parent = nullptr);

    // Everything below is on the SOURCE's clock. The panel does every
    // conversion, because it is the only thing here that knows both.

    // The full seekable extent. A live source passes what is retained.
    void setExtent(double t0, double t1);

    void setPlayhead(double t);

    // NOT setCursor(). QWidget::setCursor(QCursor) is an overload set this would
    // join, and Qt::CursorShape converts to int converts to double converts to
    // optional<double> -- so setCursor(Qt::OpenHandCursor) would silently park
    // the shared cursor at "13 seconds". It compiles, it runs, and the only
    // symptom is a stray line on every panel. OverviewStrip carries the same
    // warning after the same near miss.
    void setTimeCursor(std::optional<double> t);

    // Where the seek points are, so the track shows which instants a scrub can
    // land on exactly. Everything between two ticks is decode-forward from the
    // left one.
    //
    // Copied rather than viewed: it is regenerated from the source's index
    // whenever that changes, and a span into a vector the worker thread may
    // reallocate is the sort of thing that works until the recording is long
    // enough.
    void setSeekPoints(std::vector<double> times);

    void setPlaying(bool playing);
    bool playing() const { return playing_; }

    // Whether there is anything to scrub. A live source with no retained video,
    // or a stream with no seek points at all, draws an explanatory track rather
    // than a working-looking one that does nothing when dragged.
    void setSeekable(bool seekable);

  signals:
    // A click or a drag asked to move the playhead. The PANEL decides what that
    // means -- this never touches the time base itself.
    void seekRequested(double t);

    // Hovering moves the same shared cursor the plots set.
    void cursorRequested(std::optional<double> t);

    void playToggled(bool playing);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    QRectF trackRect() const;
    double timeAt(int x) const;
    double xFor(double t) const;

    double extent_begin_ = 0.0;
    double extent_end_ = 1.0;

    double playhead_ = 0.0;
    std::optional<double> cursor_;

    std::vector<double> seek_points_;

    bool playing_ = false;
    bool seekable_ = false;
    bool dragging_ = false;
};

}  // namespace scope

#endif  // SCOPE_VIDEO_SCRUBBER_H_
