#ifndef OPENMW_PROCESSOROBJECTMISCELLANEOUS_HPP
#define OPENMW_PROCESSOROBJECTMISCELLANEOUS_HPP

#include "../ObjectProcessor.hpp"

namespace mwmp
{
    class ProcessorObjectMiscellaneous : public ObjectProcessor
    {
    public:
        ProcessorObjectMiscellaneous()
        {
            BPP_INIT(ID_OBJECT_MISCELLANEOUS)
        }
    };
}

#endif //OPENMW_PROCESSOROBJECTMISCELLANEOUS_HPP
