#ifndef OPENMW_PROCESSORPLAYERSPELLSACTIVE_HPP
#define OPENMW_PROCESSORPLAYERSPELLSACTIVE_HPP


#include "../PlayerProcessor.hpp"

namespace mwmp
{
    class ProcessorPlayerSpellsActive final: public PlayerProcessor
    {
    public:
        ProcessorPlayerSpellsActive()
        {
            BPP_INIT(ID_PLAYER_SPELLS_ACTIVE)
        }

        virtual void Do(PlayerPacket &packet, BasePlayer *player)
        {
            if (!isLocal()) return;

            LOG_MESSAGE_SIMPLE(TimedLog::LOG_INFO, "Received ID_PLAYER_SPELLS_ACTIVE about LocalPlayer from server");

            if (isRequest())
                static_cast<LocalPlayer*>(player)->sendSpellsActive();
            else
            {
                LocalPlayer &localPlayer = static_cast<LocalPlayer&>(*player);
                
                int spellsActiveAction = localPlayer.spellsActiveChanges.action;

                if (spellsActiveAction == SpellsActiveChanges::ADD)
                    localPlayer.addSpellsActive();
                else if (spellsActiveAction == SpellsActiveChanges::REMOVE)
                    localPlayer.removeSpellsActive();
                else // SpellsActiveChanges::SET
                    localPlayer.setSpellsActive();
            }
        }
    };
}

#endif //OPENMW_PROCESSORPLAYERSPELLSACTIVE_HPP
