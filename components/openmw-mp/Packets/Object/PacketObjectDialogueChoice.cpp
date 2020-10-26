#include <components/openmw-mp/NetworkMessages.hpp>
#include "PacketObjectDialogueChoice.hpp"

using namespace mwmp;

PacketObjectDialogueChoice::PacketObjectDialogueChoice(RakNet::RakPeerInterface *peer) : ObjectPacket(peer)
{
    packetID = ID_OBJECT_DIALOGUE_CHOICE;
    hasCellData = true;
}

void PacketObjectDialogueChoice::Object(BaseObject& baseObject, bool send)
{
    ObjectPacket::Object(baseObject, send);
    RW(baseObject.dialogueChoice, send, true);
    RW(baseObject.guiId, send);
}
