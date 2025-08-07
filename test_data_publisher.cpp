#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>
#include <string>
#include <csignal>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include "zenoh.hxx"

// Cap'n Proto includes
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>
#include "vehicle_speed.capnp.h"
#include "engine_rpm.capnp.h"
#include "engine_temperature.capnp.h"
#include "vehicle_warnings.capnp.h"

class TestDataPublisher {
private:
    std::unique_ptr<zenoh::Session> mSession;
    std::unique_ptr<zenoh::Publisher> mSpeedPublisher;
    std::unique_ptr<zenoh::Publisher> mRpmPublisher;
    std::unique_ptr<zenoh::Publisher> mTemperaturePublisher;
    std::unique_ptr<zenoh::Publisher> mBatteryWarningPublisher;
    bool mRunning;
    
    // Simulation parameters
    static constexpr double PUBLISH_RATE_HZ = 30.0; // 10 Hz
    static constexpr double SPEED_CYCLE_SEC = 8.0;  // Speed cycle period
    static constexpr double RPM_CYCLE_SEC = 6.0;    // RPM cycle period
    static constexpr double TEMP_CYCLE_SEC = 12.0;  // Temperature cycle period
    static constexpr double BATTERY_CYCLE_SEC = 20.0; // Battery warning cycle period
    
public:
    TestDataPublisher() : mRunning(false) {}
    
    ~TestDataPublisher() {
        stop();
    }
    
    bool initialize() {
        try {
            // Create Zenoh configuration
            auto config = zenoh::Config::create_default();
            
            // Open Zenoh session
            mSession = std::make_unique<zenoh::Session>(zenoh::Session::open(std::move(config)));
            SPDLOG_INFO("Zenoh session opened successfully");
            
            // Create publishers for different data types
            mSpeedPublisher = std::make_unique<zenoh::Publisher>(
                mSession->declare_publisher(zenoh::KeyExpr("vehicle/speed_mps"))
            );
            
            mRpmPublisher = std::make_unique<zenoh::Publisher>(
                mSession->declare_publisher(zenoh::KeyExpr("vehicle/engine/rpm"))
            );
            
            mTemperaturePublisher = std::make_unique<zenoh::Publisher>(
                mSession->declare_publisher(zenoh::KeyExpr("vehicle/engine/temperature_celsius"))
            );
            
            mBatteryWarningPublisher = std::make_unique<zenoh::Publisher>(
                mSession->declare_publisher(zenoh::KeyExpr("vehicle/telltales/battery_warning"))
            );
            
            SPDLOG_INFO("All test data publishers created successfully");
            return true;
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to initialize Zenoh: {}", e.what());
            return false;
        }
    }
    
    void start() {
        if (!mSession) {
            SPDLOG_ERROR("Publishers not initialized. Call initialize() first.");
            return;
        }
        
        mRunning = true;
        SPDLOG_INFO("Starting test data publisher at {} Hz", PUBLISH_RATE_HZ);
        SPDLOG_INFO("Publishing to the following keys:");
        SPDLOG_INFO("  - vehicle/speed_mps (speed data in Cap'n Proto format)");
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
    void generateAndPublishData(double elapsed) {
        try {
            // Vehicle speed (0-35 m/s, sine wave pattern) - using Cap'n Proto
            double speed_phase = (elapsed / SPEED_CYCLE_SEC) * 2.0 * M_PI;
            double speed_mps = 17.5 * (1.0 + std::sin(speed_phase)); // 0-35 m/s
            publishSpeedData(mSpeedPublisher.get(), static_cast<float>(speed_mps));
            
            // Engine RPM (800-6000 RPM, different sine wave) - using Cap'n Proto
            double rpm_phase = (elapsed / RPM_CYCLE_SEC) * 2.0 * M_PI;
            double rpm = 3400 + 2600 * std::sin(rpm_phase); // 800-6000 RPM
            double oil_pressure = 40 + 20 * std::sin(rpm_phase); // 20-60 PSI
            publishRpmData(mRpmPublisher.get(), static_cast<uint32_t>(rpm), static_cast<float>(oil_pressure));
            
            // Engine temperature (60-120°C, slower variation) - using Cap'n Proto
            double temp_phase = (elapsed / TEMP_CYCLE_SEC) * 2.0 * M_PI;
            double temperature = 90 + 30 * std::sin(temp_phase); // 60-120°C
            publishTemperatureData(mTemperaturePublisher.get(), static_cast<float>(temperature));
            
            // Battery warning (periodic toggle) - using Cap'n Proto
            double battery_phase = (elapsed / BATTERY_CYCLE_SEC) * 2.0 * M_PI;
            bool battery_warning = std::sin(battery_phase) > 0.8; // Occasional warning
            publishBatteryWarningData(mBatteryWarningPublisher.get(), battery_warning);
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to generate/publish data: {}", e.what());
        }
    }
    
    
    void publishSpeedData(zenoh::Publisher* publisher, float speedMps)
    {
        if (!publisher) return;
        
        try {
            // Create a Cap'n Proto message
            ::capnp::MallocMessageBuilder message;
            VehicleSpeed::Builder vehicleSpeed = message.initRoot<VehicleSpeed>();
            
            // Set the speed value
            vehicleSpeed.setSpeedMps(speedMps);
            
            // Set timestamp (current time in milliseconds)
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            vehicleSpeed.setTimestamp(static_cast<uint64_t>(timestamp));
            
            // Use VectorOutputStream to write to its internal buffer
            kj::VectorOutputStream stream;
            capnp::writeMessage(stream, message);
            
            // Get the data from the stream (single copy instead of double copy)
            auto streamData = stream.getArray();
            std::vector<uint8_t> buffer(streamData.begin(), streamData.end());
            
            // Create Zenoh payload and publish
            publisher->put(zenoh::Bytes(std::move(buffer)));
            
            SPDLOG_DEBUG("Published speed data: {:.2f} m/s at timestamp {}", speedMps, timestamp);
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to publish speed data: {}", e.what());
        }
    }

    void publishRpmData(zenoh::Publisher* publisher, uint32_t rpm, float oil_pressure)
    {
        if (!publisher) return;
        
        try {
            // Create a Cap'n Proto message
            ::capnp::MallocMessageBuilder message;
            EngineRpm::Builder engineRpm = message.initRoot<EngineRpm>();
            
            // Set the RPM value
            engineRpm.setRpm(rpm);
            engineRpm.setOilPressurePsi(oil_pressure);

            // Set timestamp (current time in milliseconds)
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            engineRpm.setTimestamp(static_cast<uint64_t>(timestamp));
            
            // Use VectorOutputStream to write to its internal buffer
            kj::VectorOutputStream stream;
            capnp::writeMessage(stream, message);
            
            // Get the data from the stream (single copy instead of double copy)
            auto streamData = stream.getArray();
            std::vector<uint8_t> buffer(streamData.begin(), streamData.end());
            
            // Create Zenoh payload and publish
            publisher->put(zenoh::Bytes(std::move(buffer)));
            
            SPDLOG_DEBUG("Published RPM data: {} RPM at timestamp {}", rpm, timestamp);
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to publish RPM data: {}", e.what());
        }
    }

    void publishTemperatureData(zenoh::Publisher* publisher, float temperatureCelsius)
    {
        if (!publisher) return;
        
        try {
            // Create a Cap'n Proto message
            ::capnp::MallocMessageBuilder message;
            EngineTemperature::Builder engineTemp = message.initRoot<EngineTemperature>();
            
            // Set the temperature value
            engineTemp.setTemperatureCelsius(temperatureCelsius);
            
            // Set timestamp (current time in milliseconds)
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            engineTemp.setTimestamp(static_cast<uint64_t>(timestamp));
            
            // Use VectorOutputStream to write to its internal buffer
            kj::VectorOutputStream stream;
            capnp::writeMessage(stream, message);
            
            // Get the data from the stream (single copy instead of double copy)
            auto streamData = stream.getArray();
            std::vector<uint8_t> buffer(streamData.begin(), streamData.end());
            
            // Create Zenoh payload and publish
            publisher->put(zenoh::Bytes(std::move(buffer)));
            
            SPDLOG_DEBUG("Published temperature data: {:.1f}°C at timestamp {}", temperatureCelsius, timestamp);
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to publish temperature data: {}", e.what());
        }
    }

    void publishBatteryWarningData(zenoh::Publisher* publisher, bool isWarningActive)
    {
        static float modifier = 0.0f;
        if (!publisher) return;
        
        try {
            // Create a Cap'n Proto message
            ::capnp::MallocMessageBuilder message;
            BatteryWarning::Builder batteryWarning = message.initRoot<BatteryWarning>();
            
            // Set the warning status
            batteryWarning.setIsWarningActive(isWarningActive);
            batteryWarning.setBatteryVoltage(12.0f + std::sin(modifier));
            modifier += 0.01f;
            
            // Set timestamp (current time in milliseconds)
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();
            batteryWarning.setTimestamp(static_cast<uint64_t>(timestamp));
            
            // Use VectorOutputStream to write to its internal buffer
            kj::VectorOutputStream stream;
            capnp::writeMessage(stream, message);
            
            // Get the data from the stream (single copy instead of double copy)
            auto streamData = stream.getArray();
            std::vector<uint8_t> buffer(streamData.begin(), streamData.end());
            
            // Create Zenoh payload and publish
            publisher->put(zenoh::Bytes(std::move(buffer)));
            
            SPDLOG_DEBUG("Published battery warning data: {} at timestamp {}", isWarningActive ? "ACTIVE" : "INACTIVE", timestamp);
            
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to publish battery warning data: {}", e.what());
        }
    }
};

int main(int /*argc*/, char* /*argv*/[])
{
    // Set up logging
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    
    SPDLOG_INFO("Starting Mercedes Dashboard Test Data Publisher");
    SPDLOG_INFO("This will publish simulated vehicle data to demonstrate real-time widget updates");
    SPDLOG_INFO("Press Ctrl+C to stop...");
    
    TestDataPublisher publisher;
    
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