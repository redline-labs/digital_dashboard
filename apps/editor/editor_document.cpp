#include "editor/editor_document.h"

void EditorDocument::begin(EditSource source)
{
    // An edit already open from somewhere else is closed, not joined. Otherwise
    // it absorbs this one and the two come back as a single undo step -- see
    // EditSource. Committing first also means the entry it records ends at the
    // right place rather than trailing into whatever follows.
    if (pending_edit_ && pending_source_ != source)
    {
        commit();
    }

    // Nested or repeated begins collapse into the outermost one, so a drag that
    // arrives as press/move/move/release records a single entry.
    if (!pending_edit_)
    {
        pending_edit_ = owner_.captureDocument();
        pending_source_ = source;
    }
}

void EditorDocument::commit()
{
    if (!pending_edit_)
    {
        return;
    }

    const Snapshot before = *pending_edit_;
    pending_edit_.reset();

    // Nothing actually changed -- a drag that ended where it started, an Apply
    // that set every field to what it already was. Recording it would mean an
    // undo that appears to do nothing.
    if (before == owner_.captureDocument())
    {
        return;
    }

    undo_stack_.push_back(before);
    if (undo_stack_.size() > kMaxHistory)
    {
        undo_stack_.erase(undo_stack_.begin());
    }

    // A new edit invalidates the redo branch, as everywhere else.
    redo_stack_.clear();
}

bool EditorDocument::undo()
{
    if (undo_stack_.empty())
    {
        return false;
    }

    redo_stack_.push_back(owner_.captureDocument());
    const Snapshot target = undo_stack_.back();
    undo_stack_.pop_back();
    owner_.applyDocument(target);
    return true;
}

bool EditorDocument::redo()
{
    if (redo_stack_.empty())
    {
        return false;
    }

    undo_stack_.push_back(owner_.captureDocument());
    const Snapshot target = redo_stack_.back();
    redo_stack_.pop_back();
    owner_.applyDocument(target);
    return true;
}

void EditorDocument::clearHistory()
{
    undo_stack_.clear();
    redo_stack_.clear();
    pending_edit_.reset();
}

bool EditorDocument::isDirty() const
{
    return !(owner_.captureDocument() == saved_snapshot_);
}

void EditorDocument::markSaved()
{
    saved_snapshot_ = owner_.captureDocument();
}
