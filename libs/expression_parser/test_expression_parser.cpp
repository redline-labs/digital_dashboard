#include "expression_parser/expression_parser.h"
#include "expression_parser/schema_registry.h"

#include <iostream>
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
    std::cout << "=== Testing Schema Lookup by Name ===" << std::endl;
    
    // Test valid schema names
    std::vector<std::string> validSchemas = {
        "VehicleSpeed", "EngineRpm", "EngineTemperature", "BatteryWarning"
    };
    
    for (const auto& schemaName : validSchemas) {
        std::cout << "Testing schema lookup for: " << schemaName << std::endl;
        
        // Create parser with a simple expression to test schema lookup
        ExpressionParser parser(schemaName, "timestamp");
        
        // Check if schema was found and parser is valid
        assert(parser.isValid() == true);
        assert(parser.getSchemaName() == schemaName);
        
        // Verify schema was actually loaded (has non-zero ID)
        assert(parser.getSchema().getProto().getId() != 0);
        
        std::cout << "✓ Schema '" << schemaName << "' found successfully" << std::endl;
    }
    
    // Test invalid schema name
    std::cout << "Testing invalid schema lookup..." << std::endl;
    ExpressionParser invalidParser("NonExistentSchema", "timestamp");
    assert(invalidParser.isValid() == false);
    std::cout << "✓ Invalid schema correctly rejected" << std::endl;
    
    std::cout << "Schema lookup tests passed!" << std::endl << std::endl;
}

void testVariableExtraction() {
    std::cout << "=== Testing Variable Extraction from Expressions ===" << std::endl;
    
    // Test simple single variable expression
    {
        ExpressionParser parser("VehicleSpeed", "speedMps");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        assert(variables.size() == 1);
        assert(variables.find("speedMps") != variables.end());
        
        std::cout << "✓ Single variable 'speedMps' extracted correctly" << std::endl;
    }
    
    // Test multiple variable expression
    {
        ExpressionParser parser("VehicleSpeed", "speedMps * 3.6 + timestamp / 1000");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        assert(variables.size() == 2);
        assert(variables.find("speedMps") != variables.end());
        assert(variables.find("timestamp") != variables.end());
        
        std::cout << "✓ Multiple variables extracted correctly: speedMps, timestamp" << std::endl;
    }
    
    // Test complex mathematical expression
    {
        ExpressionParser parser("EngineRpm", "sqrt(rpm) + sin(timestamp / 1000.0) * 100");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        assert(variables.size() == 2);
        assert(variables.find("rpm") != variables.end());
        assert(variables.find("timestamp") != variables.end());
        
        std::cout << "✓ Complex expression variables extracted correctly" << std::endl;
    }
    
    // Test expression with repeated variables (should only extract once)
    {
        ExpressionParser parser("VehicleSpeed", "speedMps + speedMps * 2");
        assert(parser.isValid() == true);
        
        const auto& variables = parser.getVariables();
        assert(variables.size() == 1);
        assert(variables.find("speedMps") != variables.end());
        
        std::cout << "✓ Repeated variables handled correctly" << std::endl;
    }
    
    std::cout << "Variable extraction tests passed!" << std::endl << std::endl;
}

void testVariableSchemaMatching() {
    std::cout << "=== Testing Variable to Schema Field Matching ===" << std::endl;
    
    // Test valid variables that exist in schema
    {
        ExpressionParser parser("VehicleSpeed", "speedMps + timestamp");
        assert(parser.isValid() == true);
        std::cout << "✓ Valid VehicleSpeed fields matched correctly" << std::endl;
    }
    
    {
        ExpressionParser parser("EngineRpm", "rpm * 60 + timestamp");
        assert(parser.isValid() == true);
        std::cout << "✓ Valid EngineRpm fields matched correctly" << std::endl;
    }
    
    {
        ExpressionParser parser("EngineTemperature", "temperatureCelsius * 1.8 + 32");
        assert(parser.isValid() == true);
        std::cout << "✓ Valid EngineTemperature fields matched correctly" << std::endl;
    }
    
    {
        ExpressionParser parser("BatteryWarning", "isWarningActive + batteryVoltage");
        assert(parser.isValid() == true);
        std::cout << "✓ Valid BatteryWarning fields matched correctly" << std::endl;
    }
    
    // Test invalid variables that don't exist in schema
    {
        ExpressionParser parser("VehicleSpeed", "invalidField + speedMps");
        assert(parser.isValid() == false);
        std::cout << "✓ Invalid field correctly rejected" << std::endl;
    }
    
    {
        ExpressionParser parser("EngineRpm", "horsepower + rpm");
        assert(parser.isValid() == false);
        std::cout << "✓ Non-existent field correctly rejected" << std::endl;
    }
    
    // Test mixed valid/invalid variables
    {
        ExpressionParser parser("VehicleSpeed", "speedMps + nonExistentField");
        assert(parser.isValid() == false);
        std::cout << "✓ Mixed valid/invalid fields correctly rejected" << std::endl;
    }
    
    std::cout << "Variable to schema matching tests passed!" << std::endl << std::endl;
}

void testExpressionEvaluation() {
    std::cout << "=== Testing Expression Evaluation with CapnProto Payloads ===" << std::endl;
    
    // Test VehicleSpeed evaluation
    {
        std::cout << "Testing VehicleSpeed evaluation..." << std::endl;
        
        ExpressionParser parser("VehicleSpeed", "speedMps * 3.6");  // Convert m/s to km/h
        assert(parser.isValid() == true);
        
        // Create test data: 20 m/s should convert to 72 km/h
        auto payload = createVehicleSpeedMessage(20.0f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (20 * 3.6 = 72)
        assert(std::abs(result - 72.0) < 0.001);
        std::cout << "✓ VehicleSpeed: 20 m/s converted to " << result << " km/h" << std::endl;
    }
    
    // Test EngineRpm evaluation
    {
        std::cout << "Testing EngineRpm evaluation..." << std::endl;
        
        ExpressionParser parser("EngineRpm", "rpm / 1000.0");  // Convert RPM to thousands
        assert(parser.isValid() == true);
        
        // Create test data: 3500 RPM
        auto payload = createEngineRpmMessage(3500, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (3500 / 1000 = 3.5)
        assert(std::abs(result - 3.5) < 0.001);
        std::cout << "✓ EngineRpm: 3500 RPM converted to " << result << " thousands" << std::endl;
    }
    
    // Test EngineTemperature evaluation
    {
        std::cout << "Testing EngineTemperature evaluation..." << std::endl;
        
        ExpressionParser parser("EngineTemperature", "temperatureCelsius * 1.8 + 32");  // Celsius to Fahrenheit
        assert(parser.isValid() == true);
        
        // Create test data: 90°C should convert to 194°F
        auto payload = createEngineTemperatureMessage(90.0f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (90 * 1.8 + 32 = 194)
        assert(std::abs(result - 194.0) < 0.001);
        std::cout << "✓ EngineTemperature: 90°C converted to " << result << "°F" << std::endl;
    }
    
    // Test BatteryWarning evaluation (boolean + float)
    {
        std::cout << "Testing BatteryWarning evaluation..." << std::endl;
        
        ExpressionParser parser("BatteryWarning", "isWarningActive * 10 + batteryVoltage");
        assert(parser.isValid() == true);
        
        // Create test data: warning active (true = 1), battery voltage 12.6V
        auto payload = createBatteryWarningMessage(true, 12.6f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (1 * 10 + 12.6 = 22.6)
        assert(std::abs(result - 22.6) < 0.001);
        std::cout << "✓ BatteryWarning: active warning + 12.6V = " << result << std::endl;
        
        // Test with warning inactive
        payload = createBatteryWarningMessage(false, 12.6f, 1234567890ULL);
        result = parser.evaluate(payload);
        
        // Check result (0 * 10 + 12.6 = 12.6)
        assert(std::abs(result - 12.6) < 0.001);
        std::cout << "✓ BatteryWarning: inactive warning + 12.6V = " << result << std::endl;
    }
    
    // Test complex mathematical expression
    {
        std::cout << "Testing complex mathematical expression..." << std::endl;
        
        ExpressionParser parser("VehicleSpeed", "sqrt(speedMps * speedMps + 1) + sin(timestamp / 1000000.0)");
        assert(parser.isValid() == true);
        
        auto payload = createVehicleSpeedMessage(3.0f, 1000000ULL);  // 3 m/s, timestamp chosen for sin(1) ≈ 0.841
        double result = parser.evaluate(payload);
        
        // Expected: sqrt(3*3 + 1) + sin(1) = sqrt(10) + sin(1) ≈ 3.162 + 0.841 ≈ 4.003
        double expected = std::sqrt(10.0) + std::sin(1.0);
        assert(std::abs(result - expected) < 0.01);
        std::cout << "✓ Complex math expression: " << result << " (expected ~" << expected << ")" << std::endl;
    }
    
    // Test timestamp usage
    {
        std::cout << "Testing timestamp field usage..." << std::endl;
        
        ExpressionParser parser("VehicleSpeed", "timestamp / 1000000.0");  // Convert µs to seconds
        assert(parser.isValid() == true);
        
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        double result = parser.evaluate(payload);
        
        // Check result (1234567890 / 1000000 = 1234.56789)
        assert(std::abs(result - 1234.56789) < 0.001);
        std::cout << "✓ Timestamp: " << 1234567890ULL << " µs converted to " << result << " seconds" << std::endl;
    }
    
    std::cout << "Expression evaluation tests passed!" << std::endl << std::endl;
}

void testErrorHandling() {
    std::cout << "=== Testing Error Handling ===" << std::endl;
    
    // Test evaluation with invalid parser
    {
        ExpressionParser parser("VehicleSpeed", "invalidField + speedMps");
        assert(parser.isValid() == false);
        
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        
        try {
            double result = parser.evaluate(payload);
            assert(false);  // Should not reach here
        } catch (const std::runtime_error& e) {
            std::cout << "✓ Invalid parser correctly throws exception: " << e.what() << std::endl;
        }
    }
    
    // Test evaluation with wrong schema payload
    {
        ExpressionParser parser("EngineRpm", "rpm + timestamp");
        assert(parser.isValid() == true);
        
        // Try to evaluate with VehicleSpeed payload instead of EngineRpm
        auto payload = createVehicleSpeedMessage(10.0f, 1234567890ULL);
        
        try {
            double result = parser.evaluate(payload);
            // This might work or fail depending on internal CapnProto behavior
            // The important thing is it doesn't crash
            std::cout << "✓ Wrong payload schema handled gracefully" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "✓ Wrong payload schema throws exception: " << e.what() << std::endl;
        }
    }
    
    std::cout << "Error handling tests passed!" << std::endl << std::endl;
}

void printAvailableSchemas() {
    std::cout << "=== Available Schemas ===" << std::endl;
    
    auto schemas = get_available_schemas();
    for (const auto& schemaName : schemas) {
        std::cout << "- " << schemaName << std::endl;
    }
    std::cout << std::endl;
}

int main() {
    std::cout << "Expression Parser Test Program" << std::endl;
    std::cout << "==============================" << std::endl << std::endl;
    
    try {
        printAvailableSchemas();
        
        testSchemaLookup();
        testVariableExtraction();
        testVariableSchemaMatching();
        testExpressionEvaluation();
        testErrorHandling();
        
        std::cout << "🎉 All tests passed successfully!" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cout << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cout << "❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}