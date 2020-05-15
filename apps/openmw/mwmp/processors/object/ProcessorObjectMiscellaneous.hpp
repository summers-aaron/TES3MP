#ifndef OPENMW_PROCESSOROBJECTMISCELLANEOUS_HPP
#define OPENMW_PROCESSOROBJECTMISCELLANEOUS_HPP

#include "../ObjectProcessor.hpp"

namespace mwmp
{
    class ProcessorObjectMiscellaneous final: public ObjectProcessor
    {
    public:
        ProcessorObjectMiscellaneous()
        {
            BPP_INIT(ID_OBJECT_MISCELLANEOUS)
        }

        virtual void Do(ObjectPacket &packet, ObjectList &objectList)
        {
            LOG_MESSAGE_SIMPLE(TimedLog::LOG_VERBOSE, "Received %s", strPacketID.c_str());
        }
    };
}

#endif //OPENMW_PROCESSOROBJECTMISCELLANEOUS_HPP
