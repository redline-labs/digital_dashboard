#ifndef EDITOR_PAGE_EDITS_H_
#define EDITOR_PAGE_EDITS_H_

#include "editor/editor_document.h"

#include <cstddef>
#include <optional>
#include <string>

// Edits to a page_stack's pages, as functions of a document snapshot.
//
// Each takes the snapshot Canvas::mutateDocument() hands it, changes it or
// refuses, and touches nothing else -- so the rules below are tested on plain
// data, and the canvas applies the result through the same diff that undo uses.
// `stack` is the stack's index among the ACTIVE window's widgets, the window
// that `page_names` describes. A refusal leaves the snapshot unchanged.
namespace editor::page_edits
{

using Snapshot = EditorDocument::Snapshot;

// Appends a page. An empty name picks the first free `page_N`; a name already
// taken is refused. Returns the new page's index.
std::optional<std::size_t> addPage(Snapshot& state, std::size_t stack, std::string name);

// Refuses to remove the last page: a stack with none does not load. Removing
// the default page clears `default_page` so the first page is shown instead.
bool removePage(Snapshot& state, std::size_t stack, std::size_t page);

// Also renames every reference that would otherwise stop loading: the stack's
// `default_page` and triggers, and every page command aimed at this stack from
// anywhere in the document, on a page or not. Refuses an empty or taken name.
bool renamePage(Snapshot& state, std::size_t stack, std::size_t page, const std::string& name);

// False when nothing changes, so a no-op makes no undo step.
bool setPageInCycle(Snapshot& state, std::size_t stack, std::size_t page, bool in_cycle);

// Moves a page and its widgets' names together.
bool movePage(Snapshot& state, std::size_t stack, std::size_t from, std::size_t to);

// Moves the widget at `index` on page `from` to the end of page `to`, keeping
// its name, so it is still the same widget to the canvas and to undo. Refused
// when the snapshot has no name for it.
bool moveWidgetToPage(Snapshot& state, std::size_t stack, std::size_t from, std::size_t index,
                      std::size_t to);

}  // namespace editor::page_edits

#endif  // EDITOR_PAGE_EDITS_H_
