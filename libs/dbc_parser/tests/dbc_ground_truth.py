import cantools

# Load the DBC file
db = cantools.database.load_file('/Users/ryan/src/mercedes_dashboard/dbcs/motec/Motec_E888_Rev1.dbc')

# Define the message and signal values to encode
# Replace 'MyMessage' with the actual message name from your DBC file
# Replace 'Signal1', 'Signal2' with actual signal names and their desired values
data = {
    "Index0F0" : 2,
    "AV5" : 0.0,
    "AV6" : 0.0,
    "TC5" : 0.0,
    "TC6" : 0.0,
}


# Encode the message
# Replace 'MyMessage' with the actual message name
encoded_data = db.encode_message('Inputs', data)

print(f"Encoded CAN message payload: {encoded_data.hex()}")
