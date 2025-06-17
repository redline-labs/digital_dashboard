# Dynamic Multi-Window Widget Configuration

The Mercedes Dashboard now supports dynamic multi-window layout configuration through the `config.yaml` file. This allows you to define multiple windows, each with their own widgets and positioning, without recompiling the application. This is particularly useful for separating different types of content - for example, having a dedicated CarPlay window separate from an instrument cluster window.

## Configuration Structure

Add a `windows` section to your `config.yaml` file with the following structure:

```yaml
windows:
  - name: "instrument_cluster"  # Unique window identifier
    width: 800                  # Window width in pixels
    height: 450                 # Window height in pixels
    widgets:                    # Array of widgets for this window
      - type: "speedometer"
        x: 50
        y: 75
        width: 250
        height: 250
      # ... more widgets
  - name: "carplay"            # Another window
    width: 800
    height: 600
    widgets:
      - type: "carplay"
        x: 0
        y: 0
        width: 800
        height: 600
```

## Available Widget Types

The following widget types are supported:

### `speedometer`
- **Description**: Mercedes 190E style speedometer with both MPH and KM/H scales
- **Class**: `SpeedometerWidgetMPH`
- **Recommended Size**: 250x250 pixels

### `tachometer`
- **Description**: Mercedes 190E style tachometer with RPM display and clock
- **Class**: `TachometerWidget` 
- **Recommended Size**: 250x250 pixels

### `sparkline`
- **Description**: Real-time data visualization widget with sparkline graph
- **Class**: `SparklineItem`
- **Recommended Size**: 200x40 pixels

### `battery_telltale`
- **Description**: Battery warning indicator telltale
- **Class**: `BatteryTelltaleWidget`
- **Recommended Size**: 30x30 pixels

### `carplay`
- **Description**: CarPlay integration widget for phone connectivity
- **Class**: `CarPlayWidget`
- **Recommended Size**: 800x600 pixels (or full window size)
- **Note**: Only one CarPlay widget should be configured across all windows

## Window Properties

Each window requires the following properties:

- **`name`** (string): Unique identifier for the window (used in window title and logs)
- **`width`** (integer): Window width in pixels
- **`height`** (integer): Window height in pixels
- **`widgets`** (array): List of widgets to display in this window

## Widget Properties

Each widget requires the following properties:

- **`type`** (string): The widget type identifier (see available types above)
- **`x`** (integer): X position in pixels from the left edge of the window
- **`y`** (integer): Y position in pixels from the top edge of the window  
- **`width`** (integer): Widget width in pixels
- **`height`** (integer): Widget height in pixels

## Example Multi-Window Configuration

```yaml
# Multi-window layout configuration
windows:
  # Instrument cluster window with gauges and telltales
  - name: "instrument_cluster"
    width: 800
    height: 450
    widgets:
      # Mercedes 190E Speedometer on the left
      - type: "speedometer"
        x: 50
        y: 75
        width: 250
        height: 250
      
      # Mercedes 190E Tachometer on the right
      - type: "tachometer" 
        x: 350
        y: 75
        width: 250
        height: 250
      
      # Battery telltale indicator in top left
      - type: "battery_telltale"
        x: 50
        y: 25
        width: 30
        height: 30
      
      # Sparkline for engine data on top
      - type: "sparkline"
        x: 300
        y: 25
        width: 200
        height: 40

  # Dedicated CarPlay window
  - name: "carplay"
    width: 800
    height: 600
    widgets:
      - type: "carplay"
        x: 0
        y: 0
        width: 800
        height: 600
```

## Window Management

- **Multiple Windows**: Each configured window will be created as a separate Qt window
- **Window Titles**: Each window will have a title like "Mercedes Dash - instrument_cluster"
- **Independent Layout**: Each window has its own coordinate system starting at (0,0)
- **Non-Resizable**: All windows are fixed size as specified in the configuration

## Backward Compatibility

The system supports multiple levels of backward compatibility:

1. **Legacy `window` Configuration**: If you have an existing single `window` configuration, it will be automatically converted to a `windows` array with the name "main"

2. **No Window Configuration**: If no `window` or `windows` section is defined, a fallback window will be created using the legacy `width_px` and `height_px` values, but no widgets will be displayed

## Common Use Cases

### Separate CarPlay Window
Place CarPlay in its own dedicated window for external display or separate positioning:

```yaml
windows:
  - name: "carplay"
    width: 800
    height: 600
    widgets:
      - type: "carplay"
        x: 0
        y: 0
        width: 800
        height: 600
```

### Instrument Cluster Only
Create a dashboard with only instrument widgets:

```yaml
windows:
  - name: "dashboard"
    width: 600
    height: 300
    widgets:
      - type: "speedometer"
        x: 50
        y: 25
        width: 200
        height: 200
      - type: "tachometer"
        x: 300
        y: 25
        width: 200
        height: 200
```

### Multi-Monitor Setup
Configure different windows for different monitors:

```yaml
windows:
  - name: "primary_display"
    width: 1024
    height: 768
    widgets:
      # Main instrument cluster widgets
  - name: "secondary_display"  
    width: 800
    height: 480
    widgets:
      # Secondary information widgets
```

## Notes

- Widgets are positioned using absolute coordinates within each window
- Widgets can overlap if their coordinates intersect within the same window
- Each window is independently positioned by the window manager
- Invalid widget types will be logged as warnings and skipped
- CarPlay audio functionality is enabled if any window contains a CarPlay widget
- Window names should be unique to avoid confusion in logs 