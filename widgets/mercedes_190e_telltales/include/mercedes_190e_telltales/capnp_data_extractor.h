#ifndef CAPNP_DATA_EXTRACTOR_H
#define CAPNP_DATA_EXTRACTOR_H

#include <map>
#include <string>
#include <memory>
#include <functional>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <capnp/dynamic.h>
#include <capnp/schema.h>

/**
 * Generic Cap'n Proto data extractor that can work with any schema
 * and automatically extract numeric fields into a variables map for expressions
 */
class CapnpDataExtractor
{
public:
    using VariableMap = std::map<std::string, double>;
    using DataCallback = std::function<void(const VariableMap&)>;
    
    CapnpDataExtractor() = default;
    ~CapnpDataExtractor() = default;
    
    /**
     * Set the schema type for this extractor
     * @param schema_type String identifying the schema type (e.g., "BatteryWarning", "EngineRpm")
     */
    void setSchemaType(const std::string& schema_type);
    
    /**
     * Extract variables from a Cap'n Proto message payload
     * @param payload Raw bytes from Zenoh message
     * @return Map of variable names to values
     */
    VariableMap extractVariables(const std::string& payload) const;
    
    /**
     * Get list of available variable names for this schema type
     * @return Vector of variable names
     */
    std::vector<std::string> getAvailableVariables() const;
    
private:
    std::string _schema_type;
    capnp::Schema _schema;
    
    /**
     * Initialize the schema based on the schema type string
     */
    void initializeSchema();
    
    /**
     * Recursively extract numeric fields from a struct reader
     */
    void extractFromStruct(const capnp::DynamicStruct::Reader& reader, 
                          VariableMap& variables, 
                          const std::string& prefix = "") const;
    
    /**
     * Convert a field name to a valid variable name
     */
    std::string fieldToVariableName(const std::string& field_name) const;
};

#endif // CAPNP_DATA_EXTRACTOR_H 