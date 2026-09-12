// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XBus messages this tree understands.
//
// Values are transcribed from the Xsens SDK's own xstypes/xsxbusmessageid.h
// rather than from the LLCP's prose tables, so that two independent renderings
// of the protocol have to agree before anything here is trusted. The SDK
// header is the one shipped in xsens-xme-sdk; see tests/golden/README.md.
//
// Columns:
//   id      the wire byte
//   Name    the MessageId enumerator
//   snake   the display name, used in logs and in --probe output
//   dir     ToDevice, ToHost, or Both
//
// THE `Both` COLUMN IS THE WHOLE REASON THIS IS A TABLE AND NOT AN ENUM.
// Around thirty MIDs in xsxbusmessageid.h are defined twice, once as a Req and
// once as a Set, with the SAME value: ReqOutputConfiguration and
// SetOutputConfiguration are both 0xC0; ReqBaudrate and SetBaudrate are both
// 0x18. Nothing on the wire distinguishes them except the length -- a request
// carries no payload, a set does. A message id alone therefore does not name a
// message, and describe_host_message() below is what turns (id, hasPayload)
// into the name a human should see.
//
// This is deliberately NOT the whole of xsxbusmessageid.h. It is the messages
// an MTi-610 workflow touches: identification, the Config/Measurement
// transitions, the output configuration, the data message, and the two
// unsolicited reports. Wireless, bodypack, glove, factory-test and bootloader
// messages are not here and should not be added -- a row here is a claim that
// this library does something with the message.

#ifndef XBUS_MESSAGE_TABLE_H
#define XBUS_MESSAGE_TABLE_H

#define XBUS_MESSAGE_TABLE(X)                                                      \
    X(0x00, ReqDeviceId,             "req_device_id",             ToDevice)        \
    X(0x01, DeviceId,                "device_id",                 ToHost)          \
    X(0x0C, ReqConfiguration,        "req_configuration",         ToDevice)        \
    X(0x0D, Configuration,           "configuration",             ToHost)          \
    X(0x0E, RestoreFactoryDef,       "restore_factory_def",       ToDevice)        \
    X(0x0F, RestoreFactoryDefAck,    "restore_factory_def_ack",   ToHost)          \
    X(0x10, GoToMeasurement,         "goto_measurement",          ToDevice)        \
    X(0x11, GoToMeasurementAck,      "goto_measurement_ack",      ToHost)          \
    X(0x12, ReqFirmwareRevision,     "req_firmware_revision",     ToDevice)        \
    X(0x13, FirmwareRevision,        "firmware_revision",         ToHost)          \
    X(0x1C, ReqProductCode,          "req_product_code",          ToDevice)        \
    X(0x1D, ProductCode,             "product_code",              ToHost)          \
    X(0x1E, ReqHardwareVersion,      "req_hardware_version",      ToDevice)        \
    X(0x1F, HardwareVersion,         "hardware_version",          ToHost)          \
    X(0x22, SetNoRotation,           "set_no_rotation",           ToDevice)        \
    X(0x23, SetNoRotationAck,        "set_no_rotation_ack",       ToHost)          \
    X(0x24, RunSelfTest,             "run_self_test",             ToDevice)        \
    X(0x25, SelfTestResults,         "self_test_results",         ToHost)          \
    X(0x30, GoToConfig,              "goto_config",               ToDevice)        \
    X(0x31, GoToConfigAck,           "goto_config_ack",           ToHost)          \
    X(0x36, MtData2,                 "mtdata2",                   ToHost)          \
    X(0x3E, WakeUp,                  "wakeup",                    ToHost)          \
    X(0x3F, WakeUpAck,               "wakeup_ack",                ToDevice)        \
    X(0x40, Reset,                   "reset",                     ToDevice)        \
    X(0x41, ResetAck,                "reset_ack",                 ToHost)          \
    X(0x42, ErrorReport,             "error",                     ToHost)          \
    X(0x43, WarningReport,           "warning",                   ToHost)          \
    X(0x48, OptionFlags,             "option_flags",              Both)            \
    X(0x49, OptionFlagsAck,          "option_flags_ack",          ToHost)          \
    X(0x60, UtcTime,                 "utc_time",                  Both)            \
    X(0x61, UtcTimeAck,              "utc_time_ack",              ToHost)          \
    X(0x8C, PortConfig,              "port_config",               Both)            \
    X(0x8D, PortConfigAck,           "port_config_ack",           ToHost)          \
    X(0xA8, AdjustUtcTime,           "adjust_utc_time",           ToDevice)        \
    X(0xA9, AdjustUtcTimeAck,        "adjust_utc_time_ack",       ToHost)          \
    X(0xC0, OutputConfiguration,     "output_configuration",      Both)            \
    X(0xC1, OutputConfigurationAck,  "output_configuration_ack",  ToHost)          \
    X(0xEC, AlignmentRotation,       "alignment_rotation",        Both)            \
    X(0xED, AlignmentRotationAck,    "alignment_rotation_ack",    ToHost)

#endif // XBUS_MESSAGE_TABLE_H
