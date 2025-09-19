#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>
#include <string>
#include <string_view>
#include <csignal>
#include <cstdlib>
#include <spdlog/spdlog.h>

#include "helpers/unit_conversion.h"

// Cap'n Proto includes
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>
#include <optional>
#include "pub_sub/zenoh_publisher.h"
#include "vehicle_speed.capnp.h"
#include "vehicle_odometer.capnp.h"
#include "engine_rpm.capnp.h"
#include "engine_temperature.capnp.h"
#include "vehicle_warnings.capnp.h"

class TestDataPublisher {
private:
    pub_sub::ZenohPublisher<VehicleSpeed> mSpeedHelper;
    pub_sub::ZenohPublisher<VehicleOdometer> mOdometerHelper;
    pub_sub::ZenohPublisher<EngineRpm> mRpmHelper;
    pub_sub::ZenohPublisher<EngineTemperature> mTemperatureHelper;
    pub_sub::ZenohPublisher<BatteryWarning> mBatteryWarningHelper;
    bool mRunning;
    
    // Simulation parameters
    static constexpr double PUBLISH_RATE_HZ = 30.0; // 30 Hz
    static constexpr double SPEED_CYCLE_SEC = 8.0;  // Speed cycle period
    static constexpr double RPM_CYCLE_SEC = 6.0;    // RPM cycle period
    static constexpr double TEMP_CYCLE_SEC = 12.0;  // Temperature cycle period
    static constexpr double BATTERY_CYCLE_SEC = 20.0; // Battery warning cycle period
    static constexpr double ODOMETER_UPDATE_SEC = 1.0; // Odometer update period (1 Hz)
    
    // Odometer state
    mutable uint32_t mCurrentOdometer = 123456; // Starting odometer value in miles
    
public:
    TestDataPublisher(std::string_view vehicle_speed_key, std::string_view vehicle_odometer_key, std::string_view vehicle_rpm_key, std::string_view vehicle_temperature_key, std::string_view vehicle_battery_warning_key) :
        mSpeedHelper(vehicle_speed_key),
        mOdometerHelper(vehicle_odometer_key),
        mRpmHelper(vehicle_rpm_key),
        mTemperatureHelper(vehicle_temperature_key),
        mBatteryWarningHelper(vehicle_battery_warning_key),
        mRunning(false)
    {
    }
    
    ~TestDataPublisher() {
        stop();
    }
    
    bool initialize() {
        try
        {
            // Create Zenoh configuration
            //config.insert_json5("mode", "\"client\"");
            //config.insert_json5("connect/endpoints", "[\"tcp/localhost:7447\"]");


            return true;
            
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to initialize Zenoh: {}", e.what());
            return false;
        }
    }
    
    void start()
    {        
        mRunning = true;
        SPDLOG_INFO("Starting test data publisher at {} Hz", PUBLISH_RATE_HZ);
        SPDLOG_INFO("Publishing to the following keys:");
        SPDLOG_INFO("  - vehicle/speed_mps (speed data in Cap'n Proto format)");
        SPDLOG_INFO("  - vehicle/odometer (odometer data in Cap'n Proto format, starting at {} miles)", mCurrentOdometer);
        SPDLOG_INFO("  - vehicle/engine/rpm (RPM data in Cap'n Proto format)");
        SPDLOG_INFO("  - vehicle/engine/temperature_celsius (temperature data in Cap'n Proto format)");
        SPDLOG_INFO("  - vehicle/telltales/battery_warning (battery warning status in Cap'n Proto format)");
        
        const auto sleep_duration = std::chrono::milliseconds(
            static_cast<int>(1000.0 / PUBLISH_RATE_HZ)
        );
        
        auto start_time = std::chrono::steady_clock::now();
        
        while (mRunning) {
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - start_time
            ).count() / 1000.0; // Convert to seconds
            
            // Generate simulated vehicle data
            generateAndPublishData(elapsed);
            
            // Sleep to maintain the desired publish rate
            std::this_thread::sleep_for(sleep_duration);
        }
        
        SPDLOG_INFO("Test data publisher stopped");
    }
    
    void stop() {
        mRunning = false;
    }
    
private:
    static zenoh::Publisher::PutOptions capnpPutOptions(std::string_view schema)
    {
        auto opts = zenoh::Publisher::PutOptions::create_default();
        opts.encoding.emplace("application/capnp");
        opts.encoding->set_schema(schema);

        //auto ts = mSession->new_timestamp();
        //opts.timestamp = ts;
        return opts;
    }

    void generateAndPublishData(double elapsed)
    {
        try
        {
            // Vehicle speed (0-35 m/s, sine wave pattern) - using Cap'n Proto
            double speed_phase = (elapsed / SPEED_CYCLE_SEC) * 2.0 * M_PI;
            double speed_mps = 17.5 * (1.0 + std::sin(speed_phase)); // 0-35 m/s
            publishSpeedData(static_cast<float>(speed_mps));
            
            // Odometer (simulated accumulation based on speed) - using Cap'n Proto
            static double last_odometer_time = 0.0;
            if (elapsed - last_odometer_time >= ODOMETER_UPDATE_SEC)
            {
                // Calculate distance traveled since last update (speed in m/s * time in seconds * meters to miles conversion)
                double distance_miles = (mps_to_mph(speed_mps) / 3600.0) * ODOMETER_UPDATE_SEC; // m/s to miles
                distance_miles *= 50.0;  // Make it move so we can actually see it.
                mCurrentOdometer += static_cast<uint32_t>(std::round(distance_miles));
                publishOdometerData(mCurrentOdometer);

                last_odometer_time = elapsed;
            }
            
            // Engine RPM (800-6000 RPM, different sine wave) - using Cap'n Proto
            double rpm_phase = (elapsed / RPM_CYCLE_SEC) * 2.0 * M_PI;
            double rpm = 3400 + 2600 * std::sin(rpm_phase); // 800-6000 RPM
            double oil_pressure = 40 + 20 * std::sin(rpm_phase); // 20-60 PSI
            publishRpmData(static_cast<uint32_t>(rpm), static_cast<float>(oil_pressure));
            
            // Engine temperature (60-120°C, slower variation) - using Cap'n Proto
            double temp_phase = (elapsed / TEMP_CYCLE_SEC) * 2.0 * M_PI;
            double temperature = 90 + 30 * std::sin(temp_phase); // 60-120°C
            publishTemperatureData(static_cast<float>(temperature));
            
            // Battery warning (periodic toggle) - using Cap'n Proto
            double battery_phase = (elapsed / BATTERY_CYCLE_SEC) * 2.0 * M_PI;
            bool battery_warning = std::sin(battery_phase) > 0.8; // Occasional warning
            publishBatteryWarningData(battery_warning);
            
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to generate/publish data: {}", e.what());
        }
    }
    
    
    void publishSpeedData(float speedMps)
    {
        if (!mSpeedHelper.isValid()) return;
        
        try
        {
            // Set on helper builder and publish
            mSpeedHelper.fields().setSpeedMps(speedMps);
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            mSpeedHelper.fields().setTimestamp(static_cast<uint64_t>(timestamp));
            mSpeedHelper.put();
            
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to publish speed data: {}", e.what());
        }
    }

    void publishOdometerData(uint32_t totalMiles)
    {
        if (!mOdometerHelper.isValid()) return;
        
        try
        {
            mOdometerHelper.fields().setTotalMiles(totalMiles);
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            mOdometerHelper.fields().setTimestamp(static_cast<uint64_t>(timestamp));
            mOdometerHelper.put();
            
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to publish odometer data: {}", e.what());
        }
    }

    void publishRpmData(uint32_t rpm, float oil_pressure)
    {
        if (!mRpmHelper.isValid()) return;
        
        try
        {
            // Set fields on the persistent builder and publish via helper
            mRpmHelper.fields().setRpm(rpm);
            mRpmHelper.fields().setOilPressurePsi(oil_pressure);
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            mRpmHelper.fields().setTimestamp(static_cast<uint64_t>(timestamp));
            mRpmHelper.put();
            
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to publish RPM data: {}", e.what());
        }
    }

    void publishTemperatureData(float temperatureCelsius)
    {
        if (!mTemperatureHelper.isValid()) return;
        
        try
        {
            mTemperatureHelper.fields().setTemperatureCelsius(temperatureCelsius);
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            mTemperatureHelper.fields().setTimestamp(static_cast<uint64_t>(timestamp));
            mTemperatureHelper.put();

        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to publish temperature data: {}", e.what());
        }
    }

    void publishBatteryWarningData(bool isWarningActive)
    {
        static float modifier = 0.0f;
        if (!mBatteryWarningHelper.isValid()) return;
        
        try
        {
            // Set via helper builder and publish
            mBatteryWarningHelper.fields().setIsWarningActive(isWarningActive);
            mBatteryWarningHelper.fields().setBatteryVoltage(12.0f + std::sin(modifier));
            modifier += 0.01f;
            
            // Set timestamp and publish
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            mBatteryWarningHelper.fields().setTimestamp(static_cast<uint64_t>(timestamp));
            mBatteryWarningHelper.put();
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Failed to publish battery warning data: {}", e.what());
        }
    }
};

int main(int /*argc*/, char* /*argv*/[])
{
    // Set up logging
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    SPDLOG_INFO("Press Ctrl+C to stop...");
    
    TestDataPublisher publisher(
        "vehicle/speed_mps",
        "vehicle/odometer",
        "vehicle/engine/rpm",
        "vehicle/engine/temperature_celsius",
        "vehicle/telltales/battery_warning"
    );
    
    if (!publisher.initialize())
    {
        SPDLOG_ERROR("Failed to initialize publisher. Exiting.");
        return -1;
    }
    
    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, [](int /*signal*/)
    {
        SPDLOG_INFO("Received interrupt signal. Shutting down...");
        std::exit(0);
    });
    
    // Start publishing (this will run until interrupted)
    publisher.start();
    
    return 0;
} 