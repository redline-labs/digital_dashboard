#ifndef DASHBOARD_EDITOR_EDITOR_METHODS_H_
#define DASHBOARD_EDITOR_EDITOR_METHODS_H_

#include "agent_control/server.h"

class EditorWindow;

namespace editor::agent
{

// Registers the editor.* verbs: palette, add_widget, select, move, resize,
// delete, set_mode, save, load and palette_drag.
//
// These are the semantic counterpart to raw event synthesis. Both are worth
// having: input.drop exercises the real Canvas::dropEvent path an agent should
// be testing, while these are what you reach for when you want to *arrange* a
// layout rather than test the drag machinery.
void registerEditorMethods(agent_control::AgentServer& server, EditorWindow& window);

}  // namespace editor::agent

#endif  // DASHBOARD_EDITOR_EDITOR_METHODS_H_
