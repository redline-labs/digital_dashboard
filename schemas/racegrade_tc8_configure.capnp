@0xa8c3d2e1f5b76637;

struct RaceGradeTc8ConfigureRequest {
    messageFormat @0 : MessageFormat;
    transmitRate @1 : TransmitRate;
    canId @2 : UInt32;

    enum MessageFormat
    {
        e888Id0x0F0 @0;
        e888Id0x0F4 @1;
        e888Id0x0F8 @2;
        e888Id0x0FC @3;
        userSelectableStandardOutputTemperature @4;
        userSelectableStandardOutputMillivolts @5;
    }

    enum TransmitRate
    {
        rate50Hz @0;
        rate20Hz @1;
        rate10Hz @2;
        rate5Hz  @3;
        rate1Hz  @4;
        rate1over10Hz @5;
        rate1over30Hz @6;
        rate1over60Hz @7;
    }
}

struct RaceGradeTc8ConfigureResponse {
    response @0 : Bool;
}


