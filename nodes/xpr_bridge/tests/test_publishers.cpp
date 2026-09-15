// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the radio's state looks like on the bus.
//
// The channel and identity mappings are shared between the topics and the
// service replies, so getting one wrong reports a different channel on the
// topic than the service just answered with -- and zone and channel are two
// adjacent small integers, which is exactly the pair a transposition hides in.
#include "publishers.h"

#include <capnp/message.h>

#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main()
{
    {
        xpr_node::ChannelState channel;
        // Four different numbers: "channel 2 of 5 in zone 1 of 3" says nothing
        // about which field is which if they all read the same.
        channel.zone = 1;
        channel.channel = 2;
        channel.zoneCount = 3;
        channel.channelsInZone = 5;
        channel.fromBroadcast = true;

        capnp::MallocMessageBuilder message;
        xpr_node::fillChannel(message.initRoot<::XprChannel>(), channel);
        const auto out = message.getRoot<::XprChannel>().asReader();

        expect(out.getZone() == 1, "zone");
        expect(out.getChannel() == 2, "channel");
        expect(out.getZoneCount() == 3, "zone count");
        expect(out.getChannelsInZone() == 5, "channels in zone");
        expect(out.getFromBroadcast(), "where the reading came from");
    }

    {
        // A channel learned by asking, not from a broadcast: the flag is how a
        // consumer knows whether the counts are current.
        xpr_node::ChannelState channel;
        channel.fromBroadcast = false;

        capnp::MallocMessageBuilder message;
        xpr_node::fillChannel(message.initRoot<::XprChannel>(), channel);
        expect(!message.getRoot<::XprChannel>().asReader().getFromBroadcast(),
               "a queried channel does not claim to be a broadcast");
    }

    {
        xpr::Radio::Identity identity;
        identity.modelNumber = "AAH56RDN9RA1AN";
        identity.serialNumber = "123TWX4567";
        identity.firmwareVersion = "R02.21.01.0000";
        identity.tanapaNumber = "PMUD1234A";
        identity.radioId = 2607;
        identity.radioIdKnown = true;
        identity.datecode = {1, 2, 3, 4, 5, 6, 7};

        capnp::MallocMessageBuilder message;
        xpr_node::fillIdentity(message.initRoot<::XprIdentity>(), identity);
        const auto out = message.getRoot<::XprIdentity>().asReader();

        // Four strings of the same shape, next to each other: a swap between
        // model and TANAPA is invisible unless they are told apart here.
        expect(out.getModelNumber() == "AAH56RDN9RA1AN", "model number");
        expect(out.getSerialNumber() == "123TWX4567", "serial number");
        expect(out.getFirmwareVersion() == "R02.21.01.0000", "firmware version");
        expect(out.getTanapaNumber() == "PMUD1234A", "TANAPA number");

        expect(out.getRadioId() == 2607, "radio id");
        expect(out.getRadioIdKnown(), "and that it is known");

        // The datecode is bytes on purpose -- its layout is not understood --
        // so what matters is that all seven arrive in order.
        const auto datecode = out.getDatecode();
        expect(datecode.size() == identity.datecode.size(), "the whole datecode is carried");
        bool in_order = datecode.size() == identity.datecode.size();
        for (unsigned i = 0; in_order && i < datecode.size(); ++i)
        {
            in_order = datecode[i] == identity.datecode[i];
        }
        expect(in_order, "and in the order the radio gave it");
    }

    {
        // A radio that did not report its id: the flag is the difference
        // between "id 0" and "no id", and 0 is a legal DMR id.
        xpr::Radio::Identity identity;
        identity.radioId = 0;
        identity.radioIdKnown = false;

        capnp::MallocMessageBuilder message;
        xpr_node::fillIdentity(message.initRoot<::XprIdentity>(), identity);
        const auto out = message.getRoot<::XprIdentity>().asReader();
        expect(!out.getRadioIdKnown(), "an unknown radio id says so rather than reporting zero");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
