//
// (C) 2005 Vojtech Janota
// (C) 2003 Xuan Thang Nguyen
//
// This library is free software, you can redistribute it
// and/or modify
// it under  the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation;
// either version 2 of the License, or any later version.
// The library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//

#include <string.h>

#include "inet/common/INETDefs.h"

#include "inet/networklayer/mpls/MPLS.h"

#include "inet/common/ModuleAccess.h"
#include "inet/networklayer/mpls/IClassifier.h"
#include "inet/networklayer/rsvp_te/Utils.h"

// FIXME temporary fix
#include "inet/networklayer/ldp/LDP.h"
#include "inet/transportlayer/tcp_common/TCPSegment.h"
#include "inet/linklayer/common/InterfaceTag_m.h"

namespace inet {

#define ICMP_TRAFFIC    6

Define_Module(MPLS);

void MPLS::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        // interfaceTable must be initialized

        lt = getModuleFromPar<LIBTable>(par("libTableModule"), this);
        ift = getModuleFromPar<IInterfaceTable>(par("interfaceTableModule"), this);
        pct = getModuleFromPar<IClassifier>(par("classifierModule"), this);
    }
}

void MPLS::handleMessage(cMessage *msg)
{
    if (!strcmp(msg->getArrivalGate()->getName(), "ifIn")) {
        EV_INFO << "Processing message from L2: " << msg << endl;
        processPacketFromL2(msg);
    }
    else if (!strcmp(msg->getArrivalGate()->getName(), "netwIn")) {
        EV_INFO << "Processing message from L3: " << msg << endl;
        processPacketFromL3(msg);
    }
    else {
        throw cRuntimeError("unexpected message: %s", msg->getName());
    }
}

void MPLS::processPacketFromL3(cMessage *msg)
{
    using namespace tcp;

    IPv4Datagram *ipdatagram = check_and_cast<IPv4Datagram *>(msg);
    //int gateIndex = msg->getArrivalGate()->getIndex();

    // XXX temporary solution, until TCPSocket and IPv4 are extended to support nam tracing
    if (ipdatagram->getTransportProtocol() == IP_PROT_TCP) {
        TCPSegment *seg = check_and_cast<TCPSegment *>(ipdatagram->getEncapsulatedPacket());
        if (seg->getDestPort() == LDP_PORT || seg->getSrcPort() == LDP_PORT) {
            ASSERT(!ipdatagram->hasPar("color"));
            ipdatagram->addPar("color") = LDP_TRAFFIC;
        }
    }
    else if (ipdatagram->getTransportProtocol() == IP_PROT_ICMP) {
        // ASSERT(!ipdatagram->hasPar("color")); XXX this did not hold sometimes...
        if (!ipdatagram->hasPar("color"))
            ipdatagram->addPar("color") = ICMP_TRAFFIC;
    }
    // XXX end of temporary area

    labelAndForwardIPv4Datagram(ipdatagram);
}

bool MPLS::tryLabelAndForwardIPv4Datagram(IPv4Datagram *ipdatagram)
{
    LabelOpVector outLabel;
    std::string outInterface;   //FIXME set based on interfaceID
    int color;

    if (!pct->lookupLabel(ipdatagram, outLabel, outInterface, color)) {
        EV_WARN << "no mapping exists for this packet" << endl;
        return false;
    }
    int outInterfaceId = ift->getInterfaceByName(outInterface.c_str())->getInterfaceId();

    ASSERT(outLabel.size() > 0);

    MPLSPacket *mplsPacket = new MPLSPacket(ipdatagram->getName());
    mplsPacket->encapsulate(ipdatagram);
    doStackOps(mplsPacket, outLabel);

    EV_INFO << "forwarding packet to " << outInterface << endl;

    mplsPacket->addPar("color") = color;

    if (!mplsPacket->hasLabel()) {
        // yes, this may happen - if we'are both ingress and egress
        ipdatagram = check_and_cast<IPv4Datagram *>(mplsPacket->decapsulate());    // XXX FIXME superfluous encaps/decaps
        delete mplsPacket;
        ipdatagram->ensureTag<InterfaceReq>()->setInterfaceId(outInterfaceId);
        sendToL2(ipdatagram);
    }
    else {
        cObject *ctrl = ipdatagram->removeControlInfo();
        mplsPacket->setControlInfo(ctrl);
        mplsPacket->ensureTag<InterfaceReq>()->setInterfaceId(outInterfaceId);
        sendToL2(mplsPacket);
    }

    return true;
}

void MPLS::labelAndForwardIPv4Datagram(IPv4Datagram *ipdatagram)
{
    if (tryLabelAndForwardIPv4Datagram(ipdatagram))
        return;

    // handling our outgoing IPv4 traffic that didn't match any FEC/LSP
    // do not use labelAndForwardIPv4Datagram for packets arriving to ingress!

    EV_INFO << "FEC not resolved, doing regular L3 routing" << endl;

    sendToL2(ipdatagram);
}

void MPLS::doStackOps(MPLSPacket *mplsPacket, const LabelOpVector& outLabel)
{
    unsigned int n = outLabel.size();

    EV_INFO << "doStackOps: " << outLabel << endl;

    for (unsigned int i = 0; i < n; i++) {
        switch (outLabel[i].optcode) {
            case PUSH_OPER:
                mplsPacket->pushLabel(outLabel[i].label);
                break;

            case SWAP_OPER:
                ASSERT(mplsPacket->hasLabel());
                mplsPacket->swapLabel(outLabel[i].label);
                break;

            case POP_OPER:
                ASSERT(mplsPacket->hasLabel());
                mplsPacket->popLabel();
                break;

            default:
                throw cRuntimeError("Unknown MPLS OptCode %d", outLabel[i].optcode);
                break;
        }
    }
}

void MPLS::processPacketFromL2(cMessage *msg)
{
    if (MPLSPacket *mplsPacket = dynamic_cast<MPLSPacket *>(msg)) {
        processMPLSPacketFromL2(mplsPacket);
    }
    else if (IPv4Datagram *ipdatagram = dynamic_cast<IPv4Datagram *>(msg)) {
        // IPv4 datagram arrives at Ingress router. We'll try to classify it
        // and add an MPLS header

        if (!tryLabelAndForwardIPv4Datagram(ipdatagram)) {
            sendToL3(ipdatagram);
        }
    }
    else {
        throw cRuntimeError("Unknown message received");
    }
}

void MPLS::processMPLSPacketFromL2(MPLSPacket *mplsPacket)
{
    int incomingInterfaceId = mplsPacket->getMandatoryTag<InterfaceInd>()->getInterfaceId();
    InterfaceEntry *ie = ift->getInterfaceById(incomingInterfaceId);
    std::string incomingInterfaceName = ie->getName();
    ASSERT(mplsPacket->hasLabel());
    int oldLabel = mplsPacket->getTopLabel();

    EV_INFO << "Received " << mplsPacket << " from L2, label=" << oldLabel << " inInterface=" << incomingInterfaceName << endl;

    if (oldLabel == -1) {
        // This is a IPv4 native packet (RSVP/TED traffic)
        // Decapsulate the message and pass up to L3
        EV_INFO << ": decapsulating and sending up\n";

        IPv4Datagram *ipdatagram = check_and_cast<IPv4Datagram *>(mplsPacket->decapsulate());
        cObject *ctrl = mplsPacket->removeControlInfo();
        ipdatagram->setControlInfo(ctrl);
        delete mplsPacket;
        sendToL3(ipdatagram);
        return;
    }

    LabelOpVector outLabel;
    std::string outInterface;
    int color;

    bool found = lt->resolveLabel(incomingInterfaceName, oldLabel, outLabel, outInterface, color);
    if (!found) {
        EV_INFO << "discarding packet, incoming label not resolved" << endl;

        delete mplsPacket;
        return;
    }

    InterfaceEntry *outgoingInterface = ift->getInterfaceByName(outInterface.c_str());

    doStackOps(mplsPacket, outLabel);

    if (mplsPacket->hasLabel()) {
        // forward labeled packet

        EV_INFO << "forwarding packet to " << outInterface << endl;

        if (mplsPacket->hasPar("color")) {
            mplsPacket->par("color") = color;
        }
        else {
            mplsPacket->addPar("color") = color;
        }

        //ASSERT(labelIf[outgoingPort]);
        mplsPacket->ensureTag<InterfaceReq>()->setInterfaceId(outgoingInterface->getInterfaceId());
        sendToL2(mplsPacket);
    }
    else {
        // last label popped, decapsulate and send out IPv4 datagram

        EV_INFO << "decapsulating IPv4 datagram" << endl;

        IPv4Datagram *nativeIP = check_and_cast<IPv4Datagram *>(mplsPacket->decapsulate());
        cObject *ctrl = mplsPacket->removeControlInfo();
        nativeIP->setControlInfo(ctrl);
        delete mplsPacket;

        if (outgoingInterface) {
            nativeIP->ensureTag<InterfaceReq>()->setInterfaceId(outgoingInterface->getInterfaceId());
            sendToL2(nativeIP);
        }
        else {
            sendToL3(nativeIP);
        }
    }
}

void MPLS::handleRegisterInterface(const InterfaceEntry &interface, cGate *ingate)
{
    registerInterface(interface, gate("netwOut"));
}

void MPLS::handleRegisterProtocol(const Protocol& protocol, cGate *protocolGate)
{
    if (!strcmp("ifIn", protocolGate->getName())) {
        registerProtocol(protocol, gate("netwOut"));
    }
    else if (!strcmp("netwIn", protocolGate->getName())) {
        registerProtocol(protocol, gate("ifOut"));
    }
    else
        throw cRuntimeError("Unknown gate: %s", protocolGate->getName());
}

} // namespace inet

