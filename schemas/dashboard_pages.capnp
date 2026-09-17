@0xc2239310f21d79e8;

# A page_stack widget on the dashboard: which of its pages is showing, and how
# to change that. Both ride dashboard/pages/<container id>/{command,state}.

# Changes the page a page_stack shows. Sent by a page_button, the CarPlay
# widget's return button, a node, or `inspect pub` -- anything on the bus.
struct PageStackCommand {
  action @0 :Action;
  # The page name. Read only by goTo; the others ignore it.
  page @1 :Text;

  enum Action {
    # The next page in the cycle, wrapping. Pages with in_cycle false are skipped.
    next @0;
    # The previous page in the cycle, wrapping.
    prev @1;
    # The named page, cycle or not.
    goTo @2;
    # The page shown before this one. Twice in a row toggles between two pages.
    back @3;
  }
}

# What a page_stack shows, published on every change and once a second, so a
# late subscriber learns it without waiting for a press.
struct PageStackState {
  current @0 :Text;
  # Empty until the page has changed at least once.
  previous @1 :Text;
  # Position of `current` in the configured page list, for expressions, which
  # cannot compare text.
  index @2 :UInt16;
  pageCount @3 :UInt16;
}
