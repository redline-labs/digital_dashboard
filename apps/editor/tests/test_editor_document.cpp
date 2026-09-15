// The undo/redo history, on its own.
//
// This logic used to be inside Canvas, wound through the widget tree it operates
// on, so the only way to test it was to stand up a QApplication and drive real
// widgets -- which is why the interesting cases below were never covered
// directly. Against a fake document they are a few lines each. No QApplication,
// no widgets, no offscreen platform: this is a unit test.

#include "editor/editor_document.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A document that is just a window name, and counts how often it is asked for
// and put back. The counts are the point of a couple of these: "did the history
// avoid touching the document" is exactly what the diffing restore is for.
class FakeDocument : public EditorDocument::Owner
{
  public:
    EditorDocument::Snapshot captureDocument() const override
    {
        ++captures;
        EditorDocument::Snapshot state;
        state.doc.name = name;
        return state;
    }

    void applyDocument(const EditorDocument::Snapshot& state) override
    {
        ++applies;
        name = state.doc.name;
    }

    std::string name = "start";
    mutable int captures = 0;
    int applies = 0;
};

void testAnEditIsUndoneAndRedone()
{
    FakeDocument doc;
    EditorDocument history(doc);

    history.begin();
    doc.name = "edited";
    history.commit();

    check(history.canUndo(), "an edit is undoable");
    check(!history.canRedo(), "nothing to redo yet");

    check(history.undo(), "undo reports it did something");
    check(doc.name == "start", "undo restored the old value, got '" + doc.name + "'");
    check(history.canRedo(), "the undone edit can be redone");

    check(history.redo(), "redo reports it did something");
    check(doc.name == "edited", "redo restored the new value, got '" + doc.name + "'");
}

void testAnEditThatChangedNothingIsNotRecorded()
{
    FakeDocument doc;
    EditorDocument history(doc);

    history.begin();
    history.commit();
    check(!history.canUndo(), "an edit that changed nothing is not recorded");

    // The real-world version: a drag that ends where it started.
    history.begin();
    doc.name = "moved";
    doc.name = "start";
    history.commit();
    check(!history.canUndo(), "an edit that ends where it started is not recorded");
}

void testRepeatedBeginsCollapseIntoOneEntry()
{
    FakeDocument doc;
    EditorDocument history(doc);

    // A drag arrives as press, move, move, ..., release.
    history.begin();
    doc.name = "a";
    history.begin();
    doc.name = "b";
    history.begin();
    doc.name = "c";
    history.commit();

    check(history.undo(), "the drag is undoable");
    check(doc.name == "start", "one undo goes back past the whole drag, got '" + doc.name + "'");
    check(!history.canUndo(), "the drag left exactly one entry");
}

// The case that was found the hard way: an entry left open by one source
// swallowing the next edit from another. The window fields commit on
// editingFinished, which needs a focus change, and the agent interface never
// moves focus.
void testAnEditFromAnotherSourceClosesTheOpenOne()
{
    FakeDocument doc;
    EditorDocument history(doc);

    // A window field is typed into and left open.
    history.begin(EditorDocument::EditSource::Window);
    doc.name = "window-edit";

    // Now an unrelated widget edit arrives.
    history.begin(EditorDocument::EditSource::Widget);
    doc.name = "widget-edit";
    history.commit();

    check(history.undo(), "the widget edit is undone");
    check(doc.name == "window-edit",
          "undoing the widget edit leaves the window edit alone, got '" + doc.name + "'");

    check(history.undo(), "the window edit is a separate entry");
    check(doc.name == "start", "the second undo reverts the window edit, got '" + doc.name + "'");
}

void testANewEditDropsTheRedoBranch()
{
    FakeDocument doc;
    EditorDocument history(doc);

    history.begin();
    doc.name = "first";
    history.commit();
    check(history.undo(), "the first edit is undone");
    check(history.canRedo(), "it can be redone");

    history.begin();
    doc.name = "second";
    history.commit();
    check(!history.canRedo(), "a new edit invalidates the redo branch");
}

void testDirtyTracksTheLastSave()
{
    FakeDocument doc;
    EditorDocument history(doc);
    history.markSaved();

    check(!history.isDirty(), "a just-saved document is clean");

    history.begin();
    doc.name = "changed";
    history.commit();
    check(history.isDirty(), "an edit makes it dirty");

    // Undoing back to the saved state makes it clean again -- comparing against
    // the saved snapshot rather than counting edits is what buys this.
    check(history.undo(), "the edit is undone");
    check(!history.isDirty(), "undoing back to the saved state is clean again");

    history.begin();
    doc.name = "changed again";
    history.commit();
    history.markSaved();
    check(!history.isDirty(), "saving makes the current state the new baseline");
}

void testClearHistoryDropsEverything()
{
    FakeDocument doc;
    EditorDocument history(doc);

    history.begin();
    doc.name = "edited";
    history.commit();
    check(history.undo(), "there is something to undo");

    history.clearHistory();
    check(!history.canUndo() && !history.canRedo(), "clearHistory drops both stacks");

    // And an entry left open must not survive it either, or the next commit
    // records an edit that began before the clear.
    history.begin();
    doc.name = "after";
    history.clearHistory();
    history.commit();
    check(!history.canUndo(), "an open entry does not survive clearHistory");
}

// A long session must not grow without limit. The bound is 100; walk past it.
void testTheHistoryIsBounded()
{
    FakeDocument doc;
    EditorDocument history(doc);

    for (int i = 0; i < 150; ++i)
    {
        history.begin();
        doc.name = "edit-" + std::to_string(i);
        history.commit();
    }

    // Undo as far as it will go and count. The oldest entries are dropped, so
    // this stops well before "start".
    int undone = 0;
    while (history.undo())
    {
        ++undone;
    }
    check(undone == 100, "the history is bounded at 100 entries, got " + std::to_string(undone));
    check(doc.name != "start", "the oldest entries were dropped, not kept");
}

void testUndoOnAnEmptyHistoryDoesNothing()
{
    FakeDocument doc;
    EditorDocument history(doc);

    check(!history.undo(), "undo on an empty history reports it did nothing");
    check(!history.redo(), "redo on an empty history reports it did nothing");
    check(doc.applies == 0, "and it did not touch the document");
    check(doc.name == "start", "the document is untouched");
}

// commit() with nothing open is a no-op, not a crash and not a stray entry.
// PropertiesPanel::commitWindowEdits() relies on this: it fires on
// editingFinished whether or not the field was actually edited.
void testCommitWithoutBeginIsHarmless()
{
    FakeDocument doc;
    EditorDocument history(doc);

    history.commit();
    check(!history.canUndo(), "commit without begin records nothing");

    doc.name = "changed outside a transaction";
    history.commit();
    check(!history.canUndo(), "and it does not retroactively record an untracked change");
}

}  // namespace

int main()
{
    testAnEditIsUndoneAndRedone();
    testAnEditThatChangedNothingIsNotRecorded();
    testRepeatedBeginsCollapseIntoOneEntry();
    testAnEditFromAnotherSourceClosesTheOpenOne();
    testANewEditDropsTheRedoBranch();
    testDirtyTracksTheLastSave();
    testClearHistoryDropsEverything();
    testTheHistoryIsBounded();
    testUndoOnAnEmptyHistoryDoesNothing();
    testCommitWithoutBeginIsHarmless();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
