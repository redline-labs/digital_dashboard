#ifndef DASHBOARD_EDITOR_EDITOR_DOCUMENT_H
#define DASHBOARD_EDITOR_EDITOR_DOCUMENT_H

#include <QString>

#include <cstddef>
#include <optional>
#include <vector>

#include "dashboard/app_config.h"

// The editor's undo/redo history and dirty tracking, with no Qt widgets in it.
//
// This logic used to live inside Canvas, wound through the widget tree it
// operates on. It is not widget logic: collapsing repeated begins into one
// entry, closing an entry when the edit comes from somewhere else, discarding an
// entry that changed nothing, bounding the stack, and deciding whether the
// document differs from the last save are all decisions about a document. Having
// them here means they can be tested against a fake document in a few lines,
// rather than by standing up a QApplication and driving real widgets.
//
// The document itself deliberately does NOT live here. In this editor the
// widgets are the document -- geometry is the frame's geometry, and a frame owns
// the configuration it was given -- so the history asks its owner for the
// current state and hands a state back to be applied. That keeps one source of
// truth instead of introducing a second copy to be kept in step, which is the
// mistake a cached `position` field on Canvas::Item already made once.
class EditorDocument
{
  public:
    // Where an edit came from. An entry stays open across repeated begin() calls
    // from the SAME source -- that is what collapses a drag's stream of
    // mouse-moves, or a field's stream of keystrokes, into one undo step -- but a
    // begin() from a DIFFERENT source closes the open one first.
    //
    // Without that, an edit that is still open swallows the next unrelated one.
    // The window fields are the case that forced it: they commit on
    // editingFinished, which needs a focus change, and the agent interface never
    // touches focus -- so a background colour typed over the control socket
    // stayed open and merged with whatever was edited next. Two edits, one undo
    // step. Deliberately not solved with a timer: this way the boundary is exact
    // rather than a race.
    enum class EditSource { Widget, Window };

    // A point in the document's history.
    //
    // The object names ride alongside the config because they are not part of
    // it: a widget with no explicit `id:` gets a name derived from its position,
    // so rebuilding from the config alone would renumber it. A widget that
    // silently changes name when you undo breaks every selector pointing at it,
    // which is exactly what the agent verbs are used through. They are also what
    // lets a restore match a stored widget to a live one and leave it alone.
    struct Snapshot
    {
        app_config_t doc;
        std::vector<QString> names;

        // Only the document decides whether anything changed. Names follow the
        // widgets, so comparing them too would be comparing the same fact twice.
        bool operator==(const Snapshot& other) const { return doc == other.doc; }
    };

    // How the history reaches the document it is tracking. Implemented by
    // whatever holds the real state -- Canvas in the editor, a plain struct in
    // the tests.
    class Owner
    {
      public:
        virtual ~Owner() = default;

        // The document as it stands right now.
        virtual Snapshot captureDocument() const = 0;

        // Put this state back. Free to do so by whatever means costs least;
        // Canvas diffs rather than rebuilding.
        virtual void applyDocument(const Snapshot& state) = 0;
    };

    explicit EditorDocument(Owner& owner) : owner_(owner) {}

    // Wrap a mutation in begin()/commit(). commit() compares the result against
    // the snapshot and records nothing if they match, so a drag that ends where
    // it started, or an Apply that changes no field, does not fill the history
    // with no-ops.
    void begin(EditSource source = EditSource::Widget);
    void commit();

    bool canUndo() const { return !undo_stack_.empty(); }
    bool canRedo() const { return !redo_stack_.empty(); }
    bool undo();
    bool redo();

    // Drops the history. Called after a load: undoing past it would restore the
    // previous document, which is not what anyone means by undo.
    void clearHistory();

    // True when the document differs from the last save (or load).
    bool isDirty() const;
    void markSaved();

  private:
    Owner& owner_;

    std::vector<Snapshot> undo_stack_;
    std::vector<Snapshot> redo_stack_;
    std::optional<Snapshot> pending_edit_;
    EditSource pending_source_ = EditSource::Widget;
    Snapshot saved_snapshot_;

    // Bounded so a long editing session cannot grow without limit.
    static constexpr std::size_t kMaxHistory = 100;
};

#endif  // DASHBOARD_EDITOR_EDITOR_DOCUMENT_H
