# Dynamic Widget Configuration

The Mercedes Dashboard now supports dynamic widget layout configuration through the `config.yaml` file. This allows you to define which widgets appear in the main window and where they are positioned without recompiling the application.

## Configuration Structure

Add a `window` section to your `config.yaml` file with the following structure:

```yaml
window:
  width: 1024      # Window width in pixels
  height: 768      # Window height in pixels
  widgets:         # Array of widgets to display
    - type: "speedometer"
      x: 50
      y: 100
      width: 300
      height: 300
    # ... more widgets
```

## Available Widget Types

The following widget types are supported:

### `speedometer`
- **Description**: Mercedes 190E style speedometer with both MPH and KM/H scales
- **Class**: `SpeedometerWidgetMPH`
- **Recommended Size**: 300x300 pixels

### `tachometer`
- **Description**: Mercedes 190E style tachometer with RPM display and clock
- **Class**: `TachometerWidget` 
- **Recommended Size**: 300x300 pixels

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
- **Recommended Size**: 400x300 pixels
- **Note**: Only one CarPlay widget should be configured at a time

## Widget Properties

Each widget requires the following properties:

- **`type`** (string): The widget type identifier (see available types above)
- **`x`** (integer): X position in pixels from the left edge of the window
- **`y`** (integer): Y position in pixels from the top edge of the window  
- **`width`** (integer): Widget width in pixels
- **`height`** (integer): Widget height in pixels

## Example Configuration

```yaml
# Window layout configuration
window:
  width: 1024
  height: 768
  widgets:
    # Mercedes 190E Speedometer on the left
    - type: "speedometer"
      x: 50
      y: 100
      width: 300
      height: 300
    
    # Mercedes 190E Tachometer on the right  
    - type: "tachometer"
      x: 450
      y: 100
      width: 300
      height: 300
    
    # Battery telltale indicator in top left
    - type: "battery_telltale"
      x: 50
      y: 50
      width: 30
      height: 30
    
    # Sparkline for engine data on top
    - type: "sparkline"
      x: 350
      y: 50
      width: 200
      height: 40
```

## Backward Compatibility

If no `window` section is defined in the configuration, the application will fall back to using the legacy `width_px` and `height_px` values for the window size, but no widgets will be displayed.

## Notes

- Widgets are positioned using absolute coordinates within the window
- Widgets can overlap if their coordinates intersect
- The window is non-resizable and uses the exact size specified in the configuration
- Invalid widget types will be logged as warnings and skipped
- CarPlay audio functionality is only enabled if a CarPlay widget is configured 