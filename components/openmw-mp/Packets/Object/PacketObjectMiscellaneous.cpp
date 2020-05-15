#include <components/openmw-mp/NetworkMessages.hpp>
#include "PacketObjectMiscellaneous.hpp"

using namespace mwmp;

PacketObjectMiscellaneous::PacketObjectMiscellaneous(RakNet::RakPeerInterface *peer) : ObjectPacket(peer)
{
    packetID = ID_OBJECT_MISCELLANEOUS;
}

void PacketObjectMiscellaneous::Object(BaseObject &baseObject, bool send)
{

}
