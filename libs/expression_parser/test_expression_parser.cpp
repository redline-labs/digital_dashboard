#include "expression_parser/expression_parser.h"
#include "expression_parser/schema_registry.h"

#include "spdlog/spdlog.h"
#include "spdlog/fmt/ranges.h"

#include <cassert>
#include <vector>
#include <cstdint>
#include <memory>

// Include generated capnp headers for test data creation
#include "vehicle_speed.capnp.h"
#include "engine_rpm.capnp.h"
#include "engine_temperature.capnp.h"
#include "vehicle_warnings.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

using namespace expression_parser;

// Helper function to create a VehicleSpeed message
std::vector<uint8_t> createVehicleSpeedMessage(float speedMps, uint64_t timestamp) {
    capnp::MallocMessageBuilder message;
    auto vehicleSpeed = message.initRoot<VehicleSpeed>();
    
    vehicleSpeed.setSpeedMps(speedMps);
    vehicleSpeed.setTimestamp(timestamp);
    
    // Serialize to bytes
    auto words = capnp::messageToFlatArray(message);
    auto bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Helper function to create an EngineRpm message
std::vector<uint8_t> createEngineRpmMessage(uint32_t rpm, uint64_t timestamp) {
    capnp::MallocMessageBuilder message;
    auto engineRpm = message.initRoot<EngineRpm>();
    
    engineRpm.setRpm(rpm);
    engineRpm.setTimestamp(timestamp);
    
    // Serialize to bytes
    auto words = capnp::messageToFlatArray(message);
    auto bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Helper function to create an EngineTemperature message
std::vector<uint8_t> createEngineTemperatureMessage(float tempCelsius, uint64_t timestamp) {
    capnp::MallocMessageBuilder message;
    auto engineTemp = message.initRoot<EngineTemperature>();
    
    engineTemp.setTemperatureCelsius(tempCelsius);
    engineTemp.setTimestamp(timestamp);
    
    // Serialize to bytes
    auto words = capnp::messageToFlatArray(message);
    auto bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Helper function to create a BatteryWarning message
std::vector<uint8_t> createBatteryWarningMessage(bool isWarningActive, float batteryVoltage, uint64_t timestamp) {
    capnp::MallocMessageBuilder message;
    auto batteryWarning = message.initRoot<BatteryWarning>();
    
    batteryWarning.setIsWarningActive(isWarningActive);
    batteryWarning.setBatteryVoltage(batteryVoltage);
    batteryWarning.setTimestamp(timestamp);
    
    // Serialize to bytes
    auto words = capnp::messageToFlatArray(message);
    auto bytes = words.asBytes();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

void testSchemaLookup() {
    SPDLOG_INFO("=== Testing Schema Lookup by Name ===");
    
    for (const auto& schema_type : kAvailableSchemas)
    {
        SPDLOG_INFO("Testing schema lookup for: {}", reflection::enum_traits<schema_type_t>::to_string(schema_type));
        
        // Create parser with a simple expression to test schema lookup
        ExpressionParser parser(schema_type, "timestamp", "vehicle/speed_mps");
        
        // Check if schema was found and parser is valid
        assert(parser.isValid() == true);
        assert(parser.getSchemaType() == schema_type);
        
        // Verify schema was actually loaded (has non-zero ID)
        assert(parser.getSchema().getProto().getId() != 0);
        
        SPDLOG_INFO("Schema '{}' found successfully", schemaName);
    }
    
    // Test invalid schema name
    SPDLOG_INFO("Testing invalid schema lookup...");
    ExpressionParser invalidParser("NonExistentSchema", "timestamp", "vehicle/speed_mps");
    assert(invalidParser.isValid() == false);
    SPDLOG_INFO("Invalid schema correctly rejected");
    
    SPDLOG_INFO("Schema lookup tests passed!");
}

void testVariableExtraction() {
    SPDLOG_INFO("=== Testing Variable Extraction from Expressions ===");
    
    // Test simple single variable expression
    {
        ExpressionParser parser("VehicleSpeed", "speedMps", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        (void)variables;
        assert(variables.size() == 1);
        assert(variables.find("speedMps") != variables.end());
        
        SPDLOG_INFO("Single variable 'speedMps' extracted correctly");
    }
    
    // Test multiple variable expression
    {
        ExpressionParser parser("VehicleSpeed", "speedMps * 3.6 + timestamp / 1000", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        (void)variables;
        assert(variables.size() == 2);
        assert(variables.find("speedMps") != variables.end());
        assert(variables.find("timestamp") != variables.end());
        
        SPDLOG_INFO("Multiple variables extracted correctly: speedMps, timestamp");
    }
    
    // Test complex mathematical expression
    {
        ExpressionParser parser("EngineRpm", "sqrt(rpm) + sin(timestamp / 1000.0) * 100", "vehicle/engine/rpm");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        (void)variables;
        assert(variables.size() == 2);
        assert(variables.find("rpm") != variables.end());
        assert(variables.find("timestamp") != variables.end());
        
        SPDLOG_INFO("Complex expression variables extracted correctly");
    }
    
    // Test expression with repeated variables (should only extract once)
    {
        ExpressionParser parser("VehicleSpeed", "speedMps + speedMps * 2", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        (void)variables;
        assert(variables.size() == 1);
        assert(variables.find("speedMps") != variables.end());
        
        SPDLOG_INFO("Repeated variables handled correctly");
    }
    
    SPDLOG_INFO("Variable extraction tests passed!");
}

void testVariableSchemaMatching() {
    SPDLOG_INFO("=== Testing Variable to Schema Field Matching ===");
    
    // Test valid variables that exist in schema
    {
        ExpressionParser parser("VehicleSpeed", "speedMps + timestamp", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        SPDLOG_INFO("Valid VehicleSpeed fields matched correctly");
    }
    
    {
        ExpressionParser parser("EngineRpm", "rpm * 60 + timestamp", "vehicle/engine/rpm");
        assert(parser.isValid() == true);
        SPDLOG_INFO("Valid EngineRpm fields matched correctly");
    }
    
    {
        ExpressionParser parser("EngineTemperature", "temperatureCelsius * 1.8 + 32", "vehicle/engine/temperature_celsius");
        assert(parser.isValid() == true);
        SPDLOG_INFO("Valid EngineTemperature fields matched correctly");
    }
    
    {
        ExpressionParser parser("BatteryWarning", "isWarningActive + batteryVoltage", "vehicle/telltales/battery_warning");
        assert(parser.isValid() == true);
        SPDLOG_INFO("Valid BatteryWarning fields matched correctly");
    }
    
    // Test invalid variables that don't exist in schema
    {
        ExpressionParser parser("VehicleSpeed", "invalidField + speedMps", "vehicle/speed_mps");
        assert(parser.isValid() == false);
        SPDLOG_INFO("Invalid field correctly rejected");
    }
    
    {
        ExpressionParser parser("EngineRpm", "horsepower + rpm", "vehicle/engine/rpm");
        assert(parser.isValid() == false);
        SPDLOG_INFO("Non-existent field correctly rejected");
    }
    
    // Test mixed valid/invalid variables
    {
        ExpressionParser parser("VehicleSpeed", "speedMps + nonExistentField", "vehicle/speed_mps");
        assert(parser.isValid() == false);
        SPDLOG_INFO("Mixed valid/invalid fields correctly rejected");
    }
    
    SPDLOG_INFO("Variable to schema matching tests passed!");
}

void testTemplatedEvaluation() {
    SPDLOG_INFO("=== Testing Templated Expression Evaluation ===");
    
    // Test boolean return type
    {
        SPDLOG_INFO("Testing boolean evaluation...");
        
        ExpressionParser parser("BatteryWarning", "isWarningActive", "vehicle/telltales/battery_warning");
        assert(parser.isValid() == true);
        
        // Test with warning active
        auto payload = createBatteryWarningMessage(true, 12.6f, 1234567890ULL);
        bool result = parser.evaluate<bool>(payload);
        (void)result;
        assert(result == true);
        SPDLOG_INFO("Boolean evaluation: isWarningActive = true");
        
        // Test with warning inactive
        payload = createBatteryWarningMessage(false, 12.6f, 1234567890ULL);
        result = parser.evaluate<bool>(payload);
        assert(result == false);
        SPDLOG_INFO("Boolean evaluation: isWarningActive = false");
        
        // Test boolean expression with threshold
        ExpressionParser thresholdParser("BatteryWarning", "batteryVoltage > 12.0", "vehicle/telltales/battery_warning");
        assert(thresholdParser.isValid() == true);
        
        payload = createBatteryWarningMessage(false, 12.6f, 1234567890ULL);
        result = thresholdParser.evaluate<bool>(payload);
        assert(result == true);
        SPDLOG_INFO("Boolean threshold: batteryVoltage > 12.0 = true (12.6V)");
        
        payload = createBatteryWarningMessage(false, 11.5f, 1234567890ULL);
        result = thresholdParser.evaluate<bool>(payload);
        assert(result == false);
        SPDLOG_INFO("Boolean threshold: batteryVoltage > 12.0 = false (11.5V)");
    }
    
    // Test float return type
    {
        SPDLOG_INFO("Testing float evaluation...");
        
        ExpressionParser parser("VehicleSpeed", "speedMps * 3.6", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        
        auto payload = createVehicleSpeedMessage(20.0f, 1234567890ULL);
        float result = parser.evaluate<float>(payload);
        assert(std::abs(result - 72.0f) < 0.001f);
        SPDLOG_INFO("Float evaluation: 20 m/s = {} km/h", result);
    }
    
    // Test integer return type
    {
        SPDLOG_INFO("Testing integer evaluation...");
        
        ExpressionParser parser("EngineRpm", "rpm / 100", "vehicle/engine/rpm");
        assert(parser.isValid() == true);
        
        auto payload = createEngineRpmMessage(3567, 1234567890ULL);
        int result = parser.evaluate<int>(payload);
        assert(result == 36);  // 3567 / 100 = 35.67, rounded to 36
        SPDLOG_INFO("Integer evaluation: 3567 RPM / 100 = {} (rounded)", result);
    }
    
    // Test long return type
    {
        SPDLOG_INFO("Testing long evaluation...");
        
        ExpressionParser parser("VehicleSpeed", "timestamp", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        long result = parser.evaluate<long>(payload);
        assert(result == 1234567890L);
        SPDLOG_INFO("Long evaluation: timestamp = {}", result);
    }
    
    SPDLOG_INFO("Templated evaluation tests passed!");
}

void testExpressionEvaluation() {
    SPDLOG_INFO("=== Testing Expression Evaluation with CapnProto Payloads ===");
    
    // Test VehicleSpeed evaluation
    {
        SPDLOG_INFO("Testing VehicleSpeed evaluation...");
        
        ExpressionParser parser("VehicleSpeed", "speedMps * 3.6", "vehicle/speed_mps");  // Convert m/s to km/h
        assert(parser.isValid() == true);
        
        // Create test data: 20 m/s should convert to 72 km/h
        auto payload = createVehicleSpeedMessage(20.0f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (20 * 3.6 = 72)
        assert(std::abs(result - 72.0) < 0.001);
        SPDLOG_INFO("VehicleSpeed: 20 m/s converted to {} km/h", result);
    }
    
    // Test EngineRpm evaluation
    {
        SPDLOG_INFO("Testing EngineRpm evaluation...");
        
        ExpressionParser parser("EngineRpm", "rpm / 1000.0", "vehicle/engine/rpm");  // Convert RPM to thousands
        assert(parser.isValid() == true);
        
        // Create test data: 3500 RPM
        auto payload = createEngineRpmMessage(3500, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (3500 / 1000 = 3.5)
        assert(std::abs(result - 3.5) < 0.001);
        SPDLOG_INFO("EngineRpm: 3500 RPM converted to {} thousands", result);
    }
    
    // Test EngineTemperature evaluation
    {
        SPDLOG_INFO("Testing EngineTemperature evaluation...");
        
        ExpressionParser parser("EngineTemperature", "temperatureCelsius * 1.8 + 32", "vehicle/engine/temperature_celsius");  // Celsius to Fahrenheit
        assert(parser.isValid() == true);
        
        // Create test data: 90°C should convert to 194°F
        auto payload = createEngineTemperatureMessage(90.0f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (90 * 1.8 + 32 = 194)
        assert(std::abs(result - 194.0) < 0.001);
        SPDLOG_INFO("EngineTemperature: 90°C converted to {}°F", result);
    }
    
    // Test BatteryWarning evaluation (boolean + float)
    {
        SPDLOG_INFO("Testing BatteryWarning evaluation...");
        
        ExpressionParser parser("BatteryWarning", "isWarningActive * 10 + batteryVoltage", "vehicle/telltales/battery_warning");
        assert(parser.isValid() == true);
        
        // Create test data: warning active (true = 1), battery voltage 12.6V
        auto payload = createBatteryWarningMessage(true, 12.6f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (1 * 10 + 12.6 = 22.6)
        assert(std::abs(result - 22.6) < 0.001);
        SPDLOG_INFO("BatteryWarning: active warning + 12.6V = {}", result);
        
        // Test with warning inactive
        payload = createBatteryWarningMessage(false, 12.6f, 1234567890ULL);
        result = parser.evaluate(payload);
        
        // Check result (0 * 10 + 12.6 = 12.6)
        assert(std::abs(result - 12.6) < 0.001);
        SPDLOG_INFO("BatteryWarning: inactive warning + 12.6V = {}", result);
    }
    
    // Test complex mathematical expression
    {
        SPDLOG_INFO("Testing complex mathematical expression...");
        
        ExpressionParser parser("VehicleSpeed", "sqrt(speedMps * speedMps + 1) + sin(timestamp / 1000000.0)", "vehicle/speed_mps");
        assert(parser.isValid() == true);
        
        auto payload = createVehicleSpeedMessage(3.0f, 1000000ULL);  // 3 m/s, timestamp chosen for sin(1) ≈ 0.841
        double result = parser.evaluate(payload);
        
        // Expected: sqrt(3*3 + 1) + sin(1) = sqrt(10) + sin(1) ≈ 3.162 + 0.841 ≈ 4.003
        double expected = std::sqrt(10.0) + std::sin(1.0);
        assert(std::abs(result - expected) < 0.01);
        SPDLOG_INFO("Complex math expression: {} (expected ~{})", result, expected);
    }
    
    // Test timestamp usage
    {
        SPDLOG_INFO("Testing timestamp field usage...");
        
        ExpressionParser parser("VehicleSpeed", "timestamp / 1000000.0", "vehicle/speed_mps");  // Convert µs to seconds
        assert(parser.isValid() == true);
        
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (1234567890 / 1000000 = 1234.56789)
        assert(std::abs(result - 1234.56789) < 0.001);
        SPDLOG_INFO("Timestamp: {} µs converted to {} seconds", 1234567890ULL, result);
    }
    
    SPDLOG_INFO("Expression evaluation tests passed!");
}

void testErrorHandling() {
    SPDLOG_INFO("=== Testing Error Handling ===");
    
    // Test evaluation with invalid parser
    {
        ExpressionParser parser("VehicleSpeed", "invalidField + speedMps", "vehicle/speed_mps");
        assert(parser.isValid() == false);
        
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        
        try {
            double result = parser.evaluate(payload);
            (void)result;
            assert(false);  // Should not reach here
        } catch (const std::runtime_error& e) {
            SPDLOG_INFO("Invalid parser correctly throws exception: {}", e.what());
        }
    }
    
    // Test evaluation with wrong schema payload
    {
        ExpressionParser parser("EngineRpm", "rpm + timestamp", "vehicle/engine/rpm");
        assert(parser.isValid() == true);
        
        // Try to evaluate with VehicleSpeed payload instead of EngineRpm
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        
        try {
            double result = parser.evaluate(payload);
            (void)result;
            // This might work or fail depending on internal CapnProto behavior
            // The important thing is it doesn't crash
            SPDLOG_INFO("Wrong payload schema handled gracefully");
        } catch (const std::exception& e) {
            SPDLOG_INFO("Wrong payload schema throws exception: {}", e.what());
        }
    }
    
    SPDLOG_INFO("Error handling tests passed!");
}

int main()
{
    spdlog::set_level(spdlog::level::debug);
    
    try {
        SPDLOG_INFO("Available Schemas: [{}]", fmt::join(get_available_schemas(), ", "));
        
        testSchemaLookup();
        testVariableExtraction();
        testVariableSchemaMatching();
        testTemplatedEvaluation();
        testExpressionEvaluation();
        testErrorHandling();
        
        SPDLOG_INFO("All tests passed successfully!");
        return 0;
        
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Test failed with exception: {}", e.what());
        return 1;
    }
}