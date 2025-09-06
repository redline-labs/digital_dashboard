import cantools

# Load the DBC file
db = cantools.database.load_file('/Users/ryan/src/mercedes_dashboard/dbcs/tesla_icb.dbc')

# Define the message and signal values to encode
# Replace 'MyMessage' with the actual message name from your DBC file
# Replace 'Signal1', 'Signal2' with actual signal names and their desired values
data = {
    "i1_Torque_Command" : 50.0,
    "i1_Speed_Command" : 100.0,
    "i1_Direction_Command" : 0,
    "i1_Inverter_Enable": 1,
    "i1_Inverter_Discharge" : 0,
    "i1_Speed_Mode_Enable" : 0,
    "i1_Torque_Limit_Command" : -100.0,
}


# Encode the message
# Replace 'MyMessage' with the actual message name
encoded_data = db.encode_message('i1_Cmd_Msg', data)

print(f"Encoded CAN message payload: {encoded_data.hex()}")
