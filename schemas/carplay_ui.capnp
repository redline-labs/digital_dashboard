@0xff669d74c3da32da;

# The screen, between the phone and the dashboard: who wants it, and whether the
# dashboard is actually showing CarPlay.

# Something the phone did that asks the dashboard to change what it shows.
# Published by nodes/carplay on <prefix>/ui_event, once per occurrence.
struct CarPlayUiEvent {
  kind @0 :Kind;
  # The url, for appRequestedUi. Empty otherwise.
  detail @1 :Text;

  enum Kind {
    # The manufacturer tile on the CarPlay home screen was tapped: the driver
    # wants the vehicle's own screens.
    oemButton @0;
    # While CarPlay was hidden, the phone took the screen back -- Siri, or a
    # call. The dashboard should show CarPlay.
    screenRequested @1;
    # While CarPlay was hidden, the phone handed back a screen it had taken.
    screenReleased @2;
    # An app on the phone asked the head unit to open `detail`.
    appRequestedUi @3;
  }
}

# Whether the dashboard's CarPlay widget is on screen. Published by the widget on
# <prefix>/visibility on every change and once a second: the node treats a
# silence as "visible", so a dashboard that exits cannot leave the phone thinking
# the car still has its screen.
struct CarPlayVisibility {
  visible @0 :Bool;
}
