@0xc830cd45d74d1983;

# Turn-by-turn route guidance from iAP2 RouteGuidance/Maneuver updates.
# maneuverType/junctionType use Apple's iAP2 enumerations.
struct CarPlayNav {
  active             @0 :Bool;
  roadName           @1 :Text;
  afterRoadName      @2 :Text;
  destinationName    @3 :Text;
  maneuverType       @4 :UInt16;
  maneuverAngleDeg   @5 :Int16;
  junctionType       @6 :UInt16;
  distanceToManeuverM @7 :Float32;
  distanceRemainingM @8 :Float32;
  timeRemainingSec   @9 :Float32;
  etaEpochSec        @10 :UInt64;
}
