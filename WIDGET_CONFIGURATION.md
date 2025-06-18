# Dynamic Multi-Window Widget Configuration with Real-Time Data

The Mercedes Dashboard now supports dynamic multi-window layout configuration through the `config.yaml` file. This allows you to define multiple windows, each with their own widgets and positioning, without recompiling the application. Additionally, widgets can now subscribe to real-time data streams via Zenoh, enabling live updates from vehicle sensors or other data sources.

## Quick Start

To test the real-time Zenoh functionality immediately:

1. **Build the project**: `make -j8` in the build directory
2. **Start the test data publisher**: `./test_data_publisher` (publishes to all configured keys)
3. **Start the dashboard**: `./carplay_cpp` in another terminal
4. **Watch real-time updates**: The speedometer, tachometer, sparkline, and battery telltale should update automatically

The provided `config.yaml` already includes example Zenoh subscriptions for all widget types.

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
        zenoh_key: "vehicle/speed_mps"  # Optional: Zenoh subscription key
      # ... more widgets
```

## Available Widget Types

The following widget types are supported with their corresponding data expectations:

### `speedometer`
- **Description**: Mercedes 190E style speedometer with both MPH and KM/H scales
- **Class**: `SpeedometerWidgetMPH`
- **Recommended Size**: 250x250 pixels
- **Zenoh Data**: Expects speed in meters per second (m/s), automatically converted to MPH
- **Example Key**: `vehicle/speed_mps`

### `tachometer`
- **Description**: Mercedes 190E style tachometer with RPM display and clock
- **Class**: `TachometerWidget` 
- **Recommended Size**: 250x250 pixels
- **Zenoh Data**: Expects RPM value as a number (e.g., 2500.0)
- **Example Key**: `vehicle/engine/rpm`

### `sparkline`
- **Description**: Real-time data visualization widget with sparkline graph
- **Class**: `SparklineItem`
- **Recommended Size**: 200x40 pixels
- **Zenoh Data**: Expects numeric values for plotting (any units)
- **Example Key**: `vehicle/engine/temperature_celsius`

### `battery_telltale`
- **Description**: Battery warning indicator telltale
- **Class**: `BatteryTelltaleWidget`
- **Recommended Size**: 30x30 pixels
- **Zenoh Data**: Expects boolean values ("true"/"false" or "1"/"0")
- **Example Key**: `vehicle/telltales/battery_warning`

### `carplay`
- **Description**: CarPlay integration widget for phone connectivity
- **Class**: `CarPlayWidget`
- **Recommended Size**: 800x600 pixels (or full window size)
- **Zenoh Data**: Not supported (uses USB/video stream data)
- **Note**: Only one CarPlay widget should be configured across all windows

## Widget Properties

Each widget requires the following properties:

- **`type`** (string): The widget type identifier (see available types above)
- **`x`** (integer): X position in pixels from the left edge of the window
- **`y`** (integer): Y position in pixels from the top edge of the window  
- **`width`** (integer): Widget width in pixels
- **`height`** (integer): Widget height in pixels
- **`zenoh_key`** (string, optional): Zenoh subscription key for real-time data updates

## Real-Time Data with Zenoh

### Zenoh Integration

Each widget can optionally subscribe to a Zenoh key to receive real-time data updates. When a `zenoh_key` is specified:

1. The application creates a Zenoh session for each window
2. Individual subscriptions are created for each widget with a zenoh_key
3. Incoming data is automatically parsed and applied to the appropriate widget
4. Data updates are thread-safe using Qt's queued connections

### Data Formats

Different widget types expect specific data formats:

- **Numeric Values**: Speedometer, tachometer, and sparkline widgets expect numeric strings (e.g., "25.5", "2500")
- **Boolean Values**: Telltale widgets expect boolean representations ("true", "false", "1", "0")

### Error Handling

- If Zenoh session creation fails, widgets will still function without real-time data
- Invalid data formats are logged as errors but don't crash the application
- Network disconnections are handled gracefully

## Example Configuration with Zenoh

```yaml
# Multi-window layout configuration with real-time data
windows:
  # Instrument cluster window with live data
  - name: "instrument_cluster"
    width: 800
    height: 450
    widgets:
      # Speedometer with real-time vehicle speed
      - type: "speedometer"
        x: 50
        y: 75
        width: 250
        height: 250
        zenoh_key: "vehicle/speed_mps"
      
      # Tachometer with real-time engine RPM
      - type: "tachometer" 
        x: 350
        y: 75
        width: 250
        height: 250
        zenoh_key: "vehicle/engine/rpm"
      
      # Battery warning with real-time status
      - type: "battery_telltale"
        x: 50
        y: 25
        width: 30
        height: 30
        zenoh_key: "vehicle/telltales/battery_warning"
      
      # Engine temperature sparkline
      - type: "sparkline"
        x: 300
        y: 25
        width: 200
        height: 40
        zenoh_key: "vehicle/engine/temperature_celsius"

  # CarPlay window (no real-time data needed)
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

## Testing with Data Publishers

The project includes multiple test data publishers:

### Simple Speed Publisher
```bash
./speed_publisher  # Publishes only to "vehicle/speed_mps"
```

### Comprehensive Test Publisher
```bash
./test_data_publisher  # Publishes to all configured keys with realistic patterns
```

When running the test data publisher, you should see:
- Speedometer showing varying speeds (0-78 mph)
- Tachometer showing varying RPM (800-6000)
- Sparkline plotting temperature data (60-120°C)
- Battery telltale occasionally lighting up

## Window Management

- **Multiple Windows**: Each configured window will be created as a separate Qt window
- **Window Titles**: Each window will have a title like "Mercedes Dash - instrument_cluster"
- **Independent Layout**: Each window has its own coordinate system starting at (0,0)
- **Independent Zenoh Sessions**: Each window manages its own Zenoh subscriptions
- **Non-Resizable**: All windows are fixed size as specified in the configuration

## Backward Compatibility

The system supports multiple levels of backward compatibility:

1. **Legacy `window` Configuration**: If you have an existing single `window` configuration, it will be automatically converted to a `windows` array with the name "main"

2. **No Window Configuration**: If no `window` or `windows` section is defined, a fallback window will be created using the legacy `width_px` and `height_px` values, but no widgets will be displayed

3. **No Zenoh Keys**: Widgets without `zenoh_key` fields work exactly as before, without real-time data

## Common Use Cases

### Real-Time Vehicle Dashboard
Configure widgets to display live vehicle data:

```yaml
windows:
  - name: "vehicle_dash"
    width: 1024
    height: 600
    widgets:
      - type: "speedometer"
        zenoh_key: "vehicle/speed_mps"
        # ... position and size
      - type: "tachometer"
        zenoh_key: "vehicle/engine/rpm"
        # ... position and size
      - type: "sparkline"
        zenoh_key: "vehicle/fuel/consumption_rate"
        # ... position and size
```

### Multi-Monitor with Data Separation
Different windows showing different data streams:

```yaml
windows:
  - name: "engine_data"
    widgets:
      - type: "tachometer"
        zenoh_key: "vehicle/engine/rpm"
      - type: "sparkline"
        zenoh_key: "vehicle/engine/temperature"
  - name: "chassis_data"
    widgets:
      - type: "speedometer"
        zenoh_key: "vehicle/speed_mps"
      - type: "sparkline"
        zenoh_key: "vehicle/suspension/pressure"
```

## Notes

- Widgets are positioned using absolute coordinates within each window
- Widgets can overlap if their coordinates intersect within the same window
- Each window is independently positioned by the window manager
- Invalid widget types will be logged as warnings and skipped
- CarPlay audio functionality is enabled if any window contains a CarPlay widget
- Window names should be unique to avoid confusion in logs
- Zenoh subscriptions are automatically cleaned up when windows are destroyed
- Data conversion (e.g., m/s to mph) is handled automatically based on widget type 